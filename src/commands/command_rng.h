#pragma once
#include <random>

namespace tideskv {

// Per-thread generator for commands that pick at random (RANDOMKEY, HRANDFIELD, SRANDMEMBER,
// SPOP). Seeded once per thread from std::random_device.
inline std::mt19937_64& CommandRng() {
  thread_local std::mt19937_64 rng{std::random_device{}()};
  return rng;
}

}  // namespace tideskv
