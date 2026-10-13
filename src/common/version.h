#pragma once
#include <string_view>

namespace tideskv {

// Version we claim in HELLO/INFO and redis.REDIS_VERSION so client libraries enable Redis 7 behaviour.
inline constexpr std::string_view kRedisCompatVersion = "7.2.0";

// Semantic version of this TidesKV build.
std::string_view Version();

}  // namespace tideskv
