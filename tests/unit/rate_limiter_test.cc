#include "migrate/rate_limiter.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace tideskv::migrate {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

TEST(RateLimiter, PacesBytesAtTheRateWithoutABurst) {  // D3: 10 MB at 1 MiB/s takes >= 5 s
  RateLimiter limiter;
  std::atomic<bool> cancel{false};
  const auto start = Clock::now();
  for (int i = 0; i < 5; ++i)
    ASSERT_TRUE(limiter.Acquire(10000, 100000, cancel));  // 0.1 s each, the first free
  const auto elapsed = Clock::now() - start;
  EXPECT_GE(elapsed, 380ms);
  EXPECT_LT(elapsed, 2s);
}

TEST(RateLimiter, ACancelEndsTheWait) {
  RateLimiter limiter;
  std::atomic<bool> cancel{false};
  ASSERT_TRUE(limiter.Acquire(10, 1, cancel));  // the next one would wait 10 s
  std::thread canceller([&] {
    std::this_thread::sleep_for(50ms);
    cancel = true;
  });
  const auto start = Clock::now();
  EXPECT_FALSE(limiter.Acquire(10, 1, cancel));
  EXPECT_LT(Clock::now() - start, 1s);
  canceller.join();
}

}  // namespace
}  // namespace tideskv::migrate
