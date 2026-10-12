#pragma once
#include <cstdint>
#include <random>
#include <vector>

namespace tideskv {

// Positions in [0, n) for HRANDFIELD/SRANDMEMBER/SPOP (M3 plan E3), sorted ascending so one
// pass over a collection can collect them.
//  distinct: min(count, n) different positions (Floyd's algorithm), each subset equally likely.
//  otherwise: count independent uniform draws (repeats allowed); callers shuffle the elements
//  afterwards, as Redis returns repeated picks in random order.
std::vector<uint64_t> SamplePositions(uint64_t n, uint64_t count, bool distinct, std::mt19937_64& rng);

}  // namespace tideskv
