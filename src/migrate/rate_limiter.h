#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

// D3: paces chunk wire bytes at a rate re-read for every chunk (migrate-batch-rate-limit-mb), with no burst:
// each Acquire first waits until the bytes acquired before have drained at the current rate. The migrator
// thread only.
namespace tideskv::migrate {

class RateLimiter {
 public:
  // false when `cancel` became true while waiting (checked every 10 ms).
  bool Acquire(size_t bytes, uint64_t bytes_per_s, const std::atomic<bool>& cancel);

 private:
  std::chrono::steady_clock::time_point next_{};  // when everything acquired so far has drained
};

}  // namespace tideskv::migrate
