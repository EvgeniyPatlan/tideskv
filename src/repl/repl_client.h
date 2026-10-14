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
#include "repl/repl_state.h"
#include "repl/replication.h"
#include "repl/socket_io.h"
#include "resp/reply_parser.h"
#include "resp/request_parser.h"
#include "server/config.h"

// The replica side of one master link (M6a D8, D9, D11, D19): a thread that connects, runs the handshake,
// sends PSYNC and applies the master's BATCH frames through Engine::ApplyReplicated, acknowledging what
// it applied with `REPLCONF ACK <seq>`. It is the only committer on a replica: identity changes (adopting
// the master's replid) are persisted on this thread, between batches.
namespace tideskv::repl {

// The largest BATCH frame payload accepted from a master: Redis's default proto-max-bulk-len. A bigger
// frame is a protocol error and drops the link, so a hostile or broken master cannot make the replica
// buffer (or decode) more than this.
inline constexpr size_t kMaxBatchFrameBytes = size_t{512} * 1024 * 1024;

class ReplClient {
 public:
  using ConfigSnapshotFn = std::function<ConfigValues()>;
  struct Callbacks {
    std::function<ReplIdentity()> identity;  // required: the node's current identity
    // Required: makes `id` durable and the node's identity (called on the client thread, D7).
    std::function<Status(const ReplIdentity& id)> persist_identity;
    std::function<void(const std::string& replid)> on_adopt;  // optional: after a new replid is persisted
    std::function<void(ReplicaLinkState)> on_state;           // optional: every state change
  };
  struct Options {
    std::chrono::milliseconds backoff_initial{1000};  // D19: 1 s, 2 s, 4 s ... 30 s
    std::chrono::milliseconds backoff_max{30000};
    std::chrono::milliseconds ack_interval{1000};
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
  // and cancelling a connect in progress), wakes a backoff wait and joins the thread.
  void Stop();

  ReplicaLinkState state() const { return state_.load(std::memory_order_acquire); }
  int64_t last_io_ms() const { return last_io_ms_.load(std::memory_order_acquire); }  // unix ms; 0 = never
  // Unix ms when the link last went down; -1 if it never came up; 0 while it is up.
  int64_t link_down_since_ms() const { return link_down_since_ms_.load(std::memory_order_acquire); }
  const std::string& host() const { return host_; }
  uint16_t port() const { return port_; }

 private:
  using SteadyClock = std::chrono::steady_clock;

  void Run();
  Status RunLink(bool* continued);  // one connection, from connect to the link dropping
  Status Connect(Fd* out);
  bool Publish(int fd);  // false when Stop() came first
  void Unpublish();
  Status Handshake(int fd, const ConfigValues& cfg);
  Status Psync(int fd, bool* continued);
  Status AdoptReplid(std::string_view replid);
  Result<resp::ReplyNode> Command(int fd, const std::vector<std::string_view>& args);
  Result<std::string> ReadLine(int fd);
  Status Stream(int fd);
  Status ParseFrames();
  Status HandleFrame(const std::vector<std::string>& args);
  Status MaybeAck(int fd);
  void SetState(ReplicaLinkState s);
  void WaitBackoff(std::chrono::milliseconds d);
  bool stopping() const { return stop_.load(std::memory_order_acquire); }

  Engine& engine_;
  ReplLog& log_;
  ConfigSnapshotFn config_;
  Callbacks cb_;
  Options options_;
  std::string host_;
  uint16_t port_ = 0;

  // Client thread only.
  std::chrono::milliseconds timeout_{60000};  // repl-timeout of the current connection
  std::string in_;
  resp::RequestParser parser_;
  SteadyClock::time_point last_io_{};
  SteadyClock::time_point last_ack_{};
  bool ack_due_ = false;
  uint64_t acked_ = 0;

  std::atomic<ReplicaLinkState> state_{ReplicaLinkState::kConnect};
  std::atomic<int64_t> last_io_ms_{0};
  std::atomic<int64_t> link_down_since_ms_{-1};
  std::atomic<bool> stop_{false};
  std::mutex fd_mu_;
  int active_fd_ = -1;  // guarded by fd_mu_: the socket Stop() shuts down
  std::mutex backoff_mu_;
  std::condition_variable backoff_cv_;
  std::mutex join_mu_;  // Stop() from two threads joins once
  std::thread thread_;
};

}  // namespace tideskv::repl
