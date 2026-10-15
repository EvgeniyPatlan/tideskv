#include "common/debug_delay.h"

#include <gtest/gtest.h>

#include <chrono>

namespace tideskv {
namespace {

using Clock = std::chrono::steady_clock;

TEST(DebugDelay, PositiveSleepsAtLeastThatLong) {
  const auto t0 = Clock::now();
  DebugDelay(20000);
  EXPECT_GE(Clock::now() - t0, std::chrono::microseconds(20000));
}

TEST(DebugDelay, ZeroAndNegativeAreCheap) {
  const auto t0 = Clock::now();
  DebugDelay(0);
  for (int i = 0; i < 1000; ++i) DebugDelay(-1000000);  // at most one 1 µs sleep
  EXPECT_LT(Clock::now() - t0, std::chrono::milliseconds(50));
}

}  // namespace
}  // namespace tideskv
