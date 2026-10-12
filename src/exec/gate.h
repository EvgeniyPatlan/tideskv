#pragma once
#include <condition_variable>
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

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  int readers_ = 0;
  int writers_waiting_ = 0;
  bool writer_ = false;
};

}  // namespace tideskv
