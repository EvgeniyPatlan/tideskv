#pragma once
#include <cstdint>

namespace tideskv {

// Wall-clock time in unix milliseconds. Injected so expiry is testable (spec §10.3).
class Clock {
 public:
  virtual ~Clock() = default;
  virtual int64_t NowMs() const = 0;
};

class SystemClock final : public Clock {
 public:
  int64_t NowMs() const override;
};

}  // namespace tideskv
