#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "server/session.h"

namespace tideskv {

// CLIENT PAUSE / UNPAUSE (Redis 7.2 networking.c pauseClientsByClient, server.c:4146; M7a D21). Covered
// commands are postponed, never refused: Dispatch parks the call (Session::postponed) and Park() records its
// link; Unpause() or the end time resumes every parked link, which re-dispatches the call from the start
// (routing included: a write paused on a master demoted meanwhile gets MOVED). mu_ is a leaf: Resume only
// posts to the connection's io_context.
class ClientPause {
 public:
  enum class Mode : uint8_t { kOff, kWrite, kAll };
  using Clock = std::chrono::steady_clock;

  ClientPause();
  ~ClientPause();  // Shutdown()
  ClientPause(const ClientPause&) = delete;
  ClientPause& operator=(const ClientPause&) = delete;

  // A pause that is already active keeps the later end time and the stricter mode.
  void Pause(Clock::time_point end, Mode mode);
  void Unpause();     // ends the pause now and resumes every parked client
  Mode mode() const;  // the mode in force now (kOff once the end time passed); lock-free
  // Any command in kAll, a may-replicate one (write, EVAL, EVALSHA, EXEC with such) in kWrite.
  bool Blocks(bool may_replicate) const {
    const Mode m = mode();
    return m == Mode::kAll || (may_replicate && m == Mode::kWrite);
  }
  // Records a parked client; false when the pause has already ended (the caller runs the command now).
  bool Park(std::weak_ptr<ClientLink> link);
  // Parked clients still connected: INFO blocked_clients counts them (Redis BLOCKED_POSTPONE).
  size_t parked() const;
  void Shutdown();  // joins the timer; parked clients are dropped without a resume (the server stops)

 private:
  static int64_t ToNs(Clock::time_point t);
  void TimerLoop();
  static void ResumeAll(const std::vector<std::weak_ptr<ClientLink>>& links);

  std::atomic<int64_t> end_ns_{0};  // steady_clock ns since its epoch; 0 = no pause; written under mu_
  std::atomic<uint8_t> mode_{0};    // a Mode; written under mu_
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::vector<std::weak_ptr<ClientLink>> parked_;  // guarded by mu_
  bool stop_ = false;                              // guarded by mu_
  std::thread timer_;                              // last: starts after every other member exists
};

}  // namespace tideskv
