#include "common/debug_delay.h"

#include <chrono>
#include <cstdint>
#include <thread>

namespace tideskv {

void DebugDelay(int usec) {
  if (usec == 0) return;
  if (usec > 0) {
    std::this_thread::sleep_for(std::chrono::microseconds(usec));
    return;
  }
  thread_local uint64_t calls = 0;
  const auto every = static_cast<uint64_t>(-static_cast<int64_t>(usec));
  if (++calls % every == 0) std::this_thread::sleep_for(std::chrono::microseconds(1));
}

}  // namespace tideskv
