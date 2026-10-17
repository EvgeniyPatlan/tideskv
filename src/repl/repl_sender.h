#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/status.h"
#include "exec/repl_log.h"
#include "repl/replog_reader.h"
#include "repl/socket_io.h"
#include "resp/request_parser.h"
#include "storage/storage.h"

// The master side of one replica link (M6a D9, D10): a thread that streams replog batches from next_seq
// on, as `BATCH <seq> <payload>` frames, and reads `REPLCONF ACK <seq>` frames back. After a FULLRESYNC
// (M6b D2, D4) it first streams a snapshot: `LOAD <chunk>` frames, then `LOADEND <S> <replog[S]> <ops>`,
// with the batches following from S+1.
namespace tideskv::repl {

inline constexpr std::string_view kPingFrame = "*1\r\n$4\r\nPING\r\n";
std::string BatchFrame(uint64_t seq, std::string_view payload);  // D9

class ReplSender {
 public:
  struct Callbacks {
    std::function<std::chrono::milliseconds()> repl_timeout;  // read every round; null = 60 s
    std::function<int()> key_save_delay_us;  // rdb-key-save-delay, per streamed key (D14); null = 0
    std::function<void()> on_ack;            // after every valid REPLCONF ACK (WAIT, Task 12); may be null
  };

  // Takes ownership of fd. next_seq is the first seq the replica wants (it holds next_seq - 1).
  ReplSender(Storage& storage, ReplLog& log, Fd fd, uint64_t next_seq, Callbacks cb = {});
  ~ReplSender();  // Stop()
  ReplSender(const ReplSender&) = delete;
  ReplSender& operator=(const ReplSender&) = delete;

  // Before Start(): where the replica listens (INFO), and input it sent before the hand-off.
  void SetPeer(std::string ip, uint16_t listening_port);
  void SetUnread(std::string bytes);
  // Before Start(): stream this snapshot first (full sync). next_seq must be snapshot.seq + 1.
  void SetSnapshot(SyncSnapshot snapshot);

  void Start();
  // Idempotent and prompt from any thread but the sender's: shuts the socket down (waking any poll) and
  // joins the thread. The descriptor is closed by the destructor.
  void Stop();

  uint64_t acked() const { return acked_.load(std::memory_order_acquire); }  // last REPLCONF ACK seq
  // The oldest seq the replica may still ask for (D18): past its ACK, and never below where it started.
  uint64_t oldest_needed() const { return std::max(acked(), start_ - 1) + 1; }
  int64_t last_ack_ms() const { return last_ack_ms_.load(std::memory_order_acquire); }  // unix ms
  uint64_t sent() const { return sent_.load(std::memory_order_acquire); }               // last seq written
  bool alive() const { return alive_.load(std::memory_order_acquire); }  // false once the link dropped
  // Online once the replica acknowledged anything after the snapshot (D10, PF9: an ACK of S = 0 too);
  // from Start without a snapshot.
  bool online() const { return online_.load(std::memory_order_acquire); }
  const std::string& peer_ip() const { return peer_ip_; }
  uint16_t listening_port() const { return listening_port_; }

 private:
  using SteadyClock = std::chrono::steady_clock;

  void Run();
  Status Loop();
  Status StreamSnapshot();
  Result<uint64_t> StreamChunks(ReadView& view);
  Status BetweenKeys();
  Status StreamRound();
  Status ReadFrames(uint64_t last, std::string* out);
  Status PollInput();
  Status ParseInput();
  Status HandleFrame(const std::vector<std::string>& args);
  Status CheckLiveness();
  std::chrono::milliseconds Timeout() const;

  ReplLog& log_;
  Fd fd_;
  Callbacks cb_;
  const uint64_t start_;  // the first seq streamed (the replica held start_ - 1)
  std::string peer_ip_;
  uint16_t listening_port_ = 0;

  // Sender thread only (set before Start).
  std::optional<SyncSnapshot> snapshot_;  // released as soon as LOADEND is written
  ReplogReader reader_;  // where streaming continues: the replica holds reader_.next() - 1 once sent
  std::string in_;
  resp::RequestParser parser_;
  SteadyClock::time_point last_write_{};
  SteadyClock::time_point last_ack_{};

  std::atomic<uint64_t> acked_{0};
  std::atomic<uint64_t> sent_{0};
  std::atomic<int64_t> last_ack_ms_{0};
  std::atomic<bool> alive_{false};
  std::atomic<bool> online_{false};
  std::atomic<bool> stop_{false};
  std::mutex join_mu_;  // Stop() from two threads joins once
  std::thread thread_;
};

}  // namespace tideskv::repl
