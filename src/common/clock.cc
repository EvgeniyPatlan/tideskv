#include "common/clock.h"

#include <chrono>

namespace tideskv {

int64_t SystemClock::NowMs() const {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace tideskv
