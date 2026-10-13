#pragma once
#include <cstdint>
#include <limits>

namespace tideskv {

// Redis's rand.c: a drand48-compatible stream (BSD-3, Salvatore Sanfilippo), process-global and
// never reseeded per script, so math.random matches Redis (M5b H9). Not thread-safe: callers
// serialise access (ScriptEngine::mu_).
inline constexpr int32_t kRedisLrand48Max = std::numeric_limits<int32_t>::max();
int32_t RedisLrand48();
void RedisSrand48(int32_t seed);

}  // namespace tideskv
