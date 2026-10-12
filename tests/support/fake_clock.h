#pragma once
#include <atomic>
#include <cstdint>

#include "common/clock.h"

namespace tideskv::test {

// Deterministic time for expiry tests. Starts at 2023-11-14T22:13:20Z.
class FakeClock final : public Clock {
 public:
  explicit FakeClock(int64_t now_ms = 1'700'000'000'000) : now_(now_ms) {}
  int64_t NowMs() const override { return now_.load(); }
  void Set(int64_t ms) { now_ = ms; }
  void Advance(int64_t ms) { now_ += ms; }

 private:
  std::atomic<int64_t> now_;
};

}  // namespace tideskv::test
