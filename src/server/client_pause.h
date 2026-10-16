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
// commands are postponed, never refused: Dispatch asks Admit(), which parks the call's link (the session
// keeps the call in Session::postponed); Unpause() or the end time resumes every parked link, which
// re-dispatches the call from the start (routing included: a write paused on a master demoted meanwhile gets
// MOVED). mu_ is a leaf: Resume only posts to the connection's io_context.
//
// Barrier (review fix 1): Redis is single-threaded, so nothing replicates after CLIENT PAUSE ... WRITE
// replied. Here other workers and the expire reaper write concurrently, so every may-replicate command and
// every reaper commit is counted in flight from its admission to its end (EndWrite), and CLIENT PAUSE calls
// WaitForWrites() before replying. Admission and Pause() order through seq_cst atomics (in_flight_ is raised
// before the mode is read; the mode is stored before in_flight_ is read), so no write slips between them.
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

  enum class Admission : uint8_t {
    kRun,         // run it now
    kRunCounted,  // run it now; a may-replicate command, in flight until EndWrite()
    kParked,      // covered by the pause: `link` is parked and resumed when the pause ends
  };
  Admission Admit(bool may_replicate, std::weak_ptr<ClientLink> link);
  bool BeginBackgroundWrite();  // the reaper: false while any pause is active; true counts it in flight
  void EndWrite();              // ends a kRunCounted admission or a BeginBackgroundWrite
  // Blocks until no write admitted before is in flight, `own` of them excepted (the caller's own EXEC, when
  // CLIENT PAUSE runs inside it), or Shutdown(). Must not be called while holding the Engine gate.
  void WaitForWrites(int own);
  // Parked clients still connected: INFO blocked_clients counts them (Redis BLOCKED_POSTPONE).
  size_t parked() const;
  void Shutdown();  // joins the timer; parked clients are dropped without a resume (the server stops)

 private:
  static int64_t ToNs(Clock::time_point t);
  void TimerLoop();
  static void ResumeAll(const std::vector<std::weak_ptr<ClientLink>>& links);
  Admission ParkOrCount(bool may_replicate, std::weak_ptr<ClientLink> link);

  std::atomic<int64_t> end_ns_{0};     // steady_clock ns since its epoch; 0 = no pause; written under mu_
  std::atomic<uint8_t> mode_{0};       // a Mode; written under mu_
  std::atomic<int64_t> in_flight_{0};  // admitted may-replicate commands and reaper commits not yet ended
  std::atomic<int> waiters_{0};        // threads in WaitForWrites: EndWrite notifies drained_ only then
  mutable std::mutex mu_;
  std::condition_variable cv_;                     // the timer
  std::condition_variable drained_;                // WaitForWrites
  std::vector<std::weak_ptr<ClientLink>> parked_;  // guarded by mu_
  bool stop_ = false;                              // guarded by mu_
  std::thread timer_;                              // last: starts after every other member exists
};

}  // namespace tideskv
