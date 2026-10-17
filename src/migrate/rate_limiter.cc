#include "migrate/rate_limiter.h"

#include <algorithm>
#include <thread>

namespace tideskv::migrate {

bool RateLimiter::Acquire(size_t bytes, uint64_t bytes_per_s, const std::atomic<bool>& cancel) {
  using Clock = std::chrono::steady_clock;
  const auto now = Clock::now();
  next_ = std::max(next_, now);
  const Clock::time_point wait_until = next_;
  const double seconds = static_cast<double>(bytes) / static_cast<double>(std::max<uint64_t>(bytes_per_s, 1));
  next_ += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
  for (auto t = Clock::now(); t < wait_until; t = Clock::now()) {
    if (cancel.load(std::memory_order_acquire)) return false;
    std::this_thread::sleep_for(std::min<Clock::duration>(wait_until - t, std::chrono::milliseconds(10)));
  }
  return !cancel.load(std::memory_order_acquire);
}

}  // namespace tideskv::migrate
