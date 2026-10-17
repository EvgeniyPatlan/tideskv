#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/status.h"
#include "exec/engine.h"
#include "exec/repl_log.h"
#include "repl/full_sync_loader.h"
#include "repl/repl_state.h"
#include "repl/replication.h"
#include "repl/socket_io.h"
#include "resp/reply_parser.h"
#include "resp/request_parser.h"
#include "server/config.h"

// The replica side of one master link (M6a D8, D9, D11, D19): a thread that connects, runs the handshake,
// sends PSYNC and applies the master's BATCH frames through Engine::ApplyReplicated, acknowledging what
// it applied with `REPLCONF ACK <seq>`. After +FULLRESYNC it first loads the master's snapshot in place
// (FullSyncLoader, M6b D5), then streams on the same link. It is the only committer on a replica: identity
// changes (adopting the master's replid) and loads are committed on this thread, between batches.
namespace tideskv::repl {

class ReplClient {
 public:
  using ConfigSnapshotFn = std::function<ConfigValues()>;
  struct Callbacks {
    std::function<ReplIdentity()> identity;  // required: the node's current identity
    // Required: makes `id` durable and the node's identity (called on the client thread, D7).
    std::function<Status(const ReplIdentity& id)> persist_identity;
    std::function<void(const std::string& replid)> on_adopt;  // optional: after a new replid is persisted
    std::function<void(ReplicaLinkState)> on_state;           // optional: every state change
    // Optional: a load persisted `id` as the node's identity (M6b D5); publish it, do not persist it.
    std::function<void(const ReplIdentity& id)> publish_identity;
    std::function<void()> on_baseline_known;  // optional: a load cleared sys/repl_baseline_unknown
    // Optional: trims the backlog (D18). Called on the client thread between batches, at most every
    // trim_interval, so a trim never runs concurrently with an apply. A failure is logged, not fatal.
    std::function<Status()> trim_backlog;
  };
  struct Options {
    std::chrono::milliseconds backoff_initial{1000};  // D19: 1 s, 2 s, 4 s ... 30 s
    std::chrono::milliseconds backoff_max{30000};
    std::chrono::milliseconds ack_interval{1000};
    std::chrono::milliseconds trim_interval{1000};
    // F5: this node's log does not describe its data, so PSYNC never says "?" (which asks to be streamed
    // from seq 1 onto an empty node): it names the node's own replid, and the master answers FULLRESYNC.
    // The initial value only: a full sync makes the baseline known (M6b D5).
    bool baseline_unknown = false;
  };

  // config is read at each connection attempt (masterauth, masteruser, repl-timeout, port and the
  // replica-announce-* settings). The engine's ReplLog must be `log`, fenced while this client runs.
  ReplClient(Engine& engine, ReplLog& log, ConfigSnapshotFn config, Callbacks cb);
  ReplClient(Engine& engine, ReplLog& log, ConfigSnapshotFn config, Callbacks cb, Options options);
  ~ReplClient();  // Stop()
  ReplClient(const ReplClient&) = delete;
  ReplClient& operator=(const ReplClient&) = delete;

  void Start(std::string host, uint16_t port);  // once; spawns the thread and never blocks
  // Idempotent and prompt from any thread but the client's own: shuts the socket down (waking any poll,
  // and cancelling a connect in progress), wakes a backoff wait and joins the thread. One wait is not
  // interruptible: while the master's DNS name is being resolved (getaddrinfo), Stop can wait up to the
  // resolver's timeout.
  void Stop();

  ReplicaLinkState state() const { return state_.load(std::memory_order_acquire); }
  int64_t last_io_ms() const { return last_io_ms_.load(std::memory_order_acquire); }  // unix ms; 0 = never
  // Unix ms when the link last went down; -1 if it never came up; 0 while it is up.
  int64_t link_down_since_ms() const { return link_down_since_ms_.load(std::memory_order_acquire); }
  // Progress for INFO (M6b D11); all zero outside a full sync. Bytes received since +FULLRESYNC; unix ms of
  // the last read during the load; unix ms when the load started (stored just before BeginLoad raises
  // loading); LOAD payload bytes applied.
  uint64_t sync_read_bytes() const { return sync_read_bytes_.load(std::memory_order_acquire); }
  int64_t sync_last_io_ms() const { return sync_last_io_ms_.load(std::memory_order_acquire); }
  int64_t loading_start_ms() const { return loading_start_ms_.load(std::memory_order_acquire); }
  uint64_t loaded_bytes() const { return loaded_bytes_.load(std::memory_order_acquire); }
  // M7a D22: Stop() ended a full-sync load whose start txn had committed and whose LOADEND had not: the
  // keyspace was wiped (or is still partial, if the abort failed). Meaningful once Stop() has returned.
  bool stopped_mid_load() const { return stopped_mid_load_.load(std::memory_order_acquire); }

 private:
  using SteadyClock = std::chrono::steady_clock;
  using FrameHandler = std::function<Status(const std::vector<std::string>& args)>;
  // What PSYNC answered: +CONTINUE (full = false), or +FULLRESYNC <replid> <seq>.
  struct PsyncOutcome {
    bool full = false;
    std::string replid;
    uint64_t seq = 0;
  };

  void Run();
  Status RunLink(bool* continued);  // one connection, from connect to the link dropping
  // True when no full-sync load is left unfinished (AbortLoad, if needed, committed); false to retry (G1).
  bool DiscardUnfinishedLoad(std::chrono::milliseconds retry_in);
  bool Publish(int fd);  // false when Stop() came first
  void Unpublish();
  Status Handshake(int fd, const ConfigValues& cfg);
  Status Psync(int fd, PsyncOutcome* out);
  Status FullSync(int fd, const PsyncOutcome& o);        // the load, until LOADEND committed
  Status LoadUntilDone(int fd, FullSyncLoader& loader);  // reads and loads frames until LOADEND
  Status ParseLoadFrames(FullSyncLoader& loader, bool* done);
  FullSyncLoader::Hooks LoaderHooks();
  void ResetSyncProgress();
  Status AdoptReplid(std::string_view replid);
  Result<resp::ReplyNode> Command(int fd, const std::vector<std::string_view>& args);
  Result<std::string> ReadLine(int fd);
  Status Stream(int fd);
  Status ParseFrames();
  // Hands each complete frame in in_ to handle, until in_ holds no complete frame or *stop (if given)
  // is set; what follows stays in in_ for the next caller. Bounds the unparsed input.
  Status DrainFrames(const FrameHandler& handle, const bool* stop);
  Status HandleFrame(const std::vector<std::string>& args);
  Status MaybeAck(int fd);
  void MaybeTrim();
  void SetState(ReplicaLinkState s);
  void WaitBackoff(std::chrono::milliseconds d);
  bool stopping() const { return stop_.load(std::memory_order_acquire); }

  Engine& engine_;
  ReplLog& log_;
  ConfigSnapshotFn config_;
  Callbacks cb_;
  Options options_;
  std::string host_;  // set by Start before the thread exists; read only by the thread
  uint16_t port_ = 0;

  // Client thread only.
  std::chrono::milliseconds timeout_{60000};  // repl-timeout of the current connection
  std::string in_;
  resp::RequestParser parser_;
  SteadyClock::time_point last_io_{};
  SteadyClock::time_point last_ack_{};
  SteadyClock::time_point last_trim_{};
  bool ack_due_ = false;
  uint64_t acked_ = 0;

  std::atomic<ReplicaLinkState> state_{ReplicaLinkState::kConnect};
  std::atomic<int64_t> last_io_ms_{0};
  std::atomic<int64_t> link_down_since_ms_{-1};
  std::atomic<bool> baseline_unknown_;  // F5, from Options; cleared by a load's start txn
  std::atomic<int> key_load_delay_{0};  // key-load-delay, read at each connection attempt
  std::atomic<uint64_t> sync_read_bytes_{0};
  std::atomic<int64_t> sync_last_io_ms_{0};
  std::atomic<int64_t> loading_start_ms_{0};
  std::atomic<uint64_t> loaded_bytes_{0};
  std::atomic<bool> stop_{false};
  std::atomic<bool> stopped_mid_load_{false};
  std::mutex fd_mu_;
  int active_fd_ = -1;  // guarded by fd_mu_: the socket Stop() shuts down
  std::mutex backoff_mu_;
  std::condition_variable backoff_cv_;
  std::mutex join_mu_;  // Stop() from two threads joins once
  std::thread thread_;
};

}  // namespace tideskv::repl
