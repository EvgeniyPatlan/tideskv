#pragma once

namespace tideskv {

// Test-only throttle, Redis debugDelay (debug.c): usec > 0 sleeps that many microseconds; usec < 0
// sleeps 1 us on every (-usec)-th call on this thread (Redis draws at random; deterministic here);
// 0 does nothing.
void DebugDelay(int usec);

}  // namespace tideskv
