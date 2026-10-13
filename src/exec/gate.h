#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>

namespace tideskv {

// Reader/writer gate with writer priority: once a writer waits, new readers queue
// behind it. Shared = normal commands, exclusive = FLUSHDB/FLUSHALL (M2 plan D6).
// Not re-entrant: never take it twice on one thread.
class Gate {
 public:
  void lock();
  void unlock();
  void lock_shared();
  void unlock_shared();
  bool try_lock();         // succeeds iff no reader or writer holds the gate
  bool try_lock_shared();  // succeeds iff no writer holds or waits for the gate
  // lock_shared()/lock() that give up and return false once give_up() is true. give_up is checked
  // when waiting starts and after every wake-up (Wake(), an unlock); it must be cheap and must not
  // touch the gate (M5b H6), and must not throw (a throw from lock_unless would leak writers_waiting_
  // and stall every reader). Whoever makes it true must call Wake() afterwards.
  bool lock_shared_unless(const std::function<bool()>& give_up);
  bool lock_unless(const std::function<bool()>& give_up);
  void Wake();  // waiters re-check give_up

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  int readers_ = 0;
  int writers_waiting_ = 0;
  bool writer_ = false;
};

}  // namespace tideskv
