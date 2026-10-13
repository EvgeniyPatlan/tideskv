#include "script/rand48.h"

#include <gtest/gtest.h>

namespace tideskv {
namespace {

// One test only: the stream is process-global and starts at drand48's fixed seed.
TEST(Rand48, MatchesRedisFromAFreshProcessAndAfterSeeding) {
  EXPECT_EQ(RedisLrand48(), 851401618);
  EXPECT_EQ(RedisLrand48(), 1804928587);
  EXPECT_EQ(RedisLrand48(), 758783491);
  RedisSrand48(1);
  EXPECT_EQ(RedisLrand48(), 89400484);
}

}  // namespace
}  // namespace tideskv
