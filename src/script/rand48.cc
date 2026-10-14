#include "script/rand48.h"

#include <cstdint>

namespace tideskv {
namespace {

constexpr uint64_t kMask48 = (uint64_t{1} << 48) - 1;
constexpr uint64_t kMultiplier = 0x5DEECE66DULL;
constexpr uint64_t kIncrement = 0xB;
constexpr uint64_t kInitialState = 0x1234ABCD330EULL;  // drand48's initial X (rand.c X0..X2)
uint64_t g_state = kInitialState;

}  // namespace

int32_t RedisLrand48() {
  g_state = (kMultiplier * g_state + kIncrement) & kMask48;
  return static_cast<int32_t>(g_state >> 17);
}

void RedisSrand48(int32_t seed) {
  g_state = ((static_cast<uint64_t>(static_cast<uint32_t>(seed)) << 16) | 0x330EULL) & kMask48;
}

void RedisRand48ResetForTests() { g_state = kInitialState; }

}  // namespace tideskv
