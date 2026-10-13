#include "script/rand48.h"

#include <gtest/gtest.h>

namespace tideskv {
namespace {

// The stream is process-global and starts at drand48's fixed seed.
TEST(Rand48, MatchesRedisFromAFreshProcessAndAfterSeeding) {
  RedisRand48ResetForTests();  // other tests in this binary may have used the stream
  EXPECT_EQ(RedisLrand48(), 851401618);
  EXPECT_EQ(RedisLrand48(), 1804928587);
  EXPECT_EQ(RedisLrand48(), 758783491);
  RedisSrand48(1);
  EXPECT_EQ(RedisLrand48(), 89400484);
}

}  // namespace
}  // namespace tideskv
