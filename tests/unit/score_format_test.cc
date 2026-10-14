#include "common/score_format.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace tideskv {
namespace {

// Expected texts from Redis 7.2.5 d2string (redis72-zset.md §0.8). If a row disagrees, confirm with
// `docker run --rm redis:7.2.5` (ZADD k <score> m; ZSCORE k m) before changing anything.
TEST(FormatScore, RedisD2string) {
  const std::pair<double, std::string> cases[] = {
      {1, "1"},
      {-3, "-3"},
      {0.1, "0.1"},
      {1.5, "1.5"},
      {3.14, "3.14"},
      {1e-5, "0.00001"},
      {1e-7, "1e-7"},
      {1e18, "1000000000000000000"},
      {4611686018427387904.0, "4611686018427387904"},  // 2^62: still the integer path
      {5e18, "5e+18"},                                 // above 2^62: fpconv
      {1e19, "1e+19"},
      {1e21, "1e+21"},
      {1.2345678901234567e19, "12345678901234567000"},
      {123456789012345678.0, "123456789012345680"},
      {0.1 + 0.2, "0.30000000000000004"},
      {1.7976931348623157e308, "1.7976931348623157e+308"},
      {5e-324, "5e-324"},
      {std::numeric_limits<double>::infinity(), "inf"},
      {-std::numeric_limits<double>::infinity(), "-inf"},
      {0.0, "0"},
      {-0.0, "-0"},
  };
  for (const auto& [v, want] : cases) EXPECT_EQ(FormatScore(v), want) << v;
  EXPECT_EQ(FormatScore(std::numeric_limits<double>::quiet_NaN()), "nan");
}

TEST(FormatScore, RedisLd2stringAuto) {
  EXPECT_EQ(FormatScore17g(0.1), "0.10000000000000001");
  EXPECT_EQ(FormatScore17g(1.5), "1.5");
  EXPECT_EQ(FormatScore17g(1), "1");
  EXPECT_EQ(FormatScore17g(1e17), "1e+17");
  EXPECT_EQ(FormatScore17g(std::numeric_limits<double>::infinity()), "inf");
  EXPECT_EQ(FormatScore17g(-std::numeric_limits<double>::infinity()), "-inf");
  EXPECT_EQ(FormatScore17g(std::numeric_limits<double>::quiet_NaN()), "nan");
  EXPECT_EQ(FormatScore17g(-0.0), "-0");
}

// The score text DEBUG DIGEST hashes (debug.c xorObjectDigest calls fpconv_dtoa directly). Expected
// texts recovered from redis:7.2.5 DEBUG DIGEST-VALUE (see cmd_debug_test ZsetScoresUseFpconvText).
TEST(FormatDtoa, RedisFpconvDtoa) {
  EXPECT_EQ(FormatDtoa(2), "2");
  EXPECT_EQ(FormatDtoa(1.5), "1.5");
  EXPECT_EQ(FormatDtoa(0.1), "0.1");
  EXPECT_EQ(FormatDtoa(100), "100");
  EXPECT_EQ(FormatDtoa(1e16), "1e+16");  // FormatScore: 10000000000000000
  EXPECT_EQ(FormatDtoa(1e20), "1e+20");
  EXPECT_EQ(FormatDtoa(3e-7), "3e-7");
  EXPECT_EQ(FormatDtoa(123456789012345678.0), "123456789012345680");
  EXPECT_EQ(FormatDtoa(std::numeric_limits<double>::infinity()), "inf");
  EXPECT_EQ(FormatDtoa(-std::numeric_limits<double>::infinity()), "-inf");
  EXPECT_EQ(FormatDtoa(0.0), "0");
  EXPECT_EQ(FormatDtoa(-0.0), "-0");
}

TEST(ParseScore, RedisString2d) {
  double d = 0;
  for (const char* ok :
       {"inf", "-inf", "+inf", "INF", "Infinity", "1e5", ".5", "5.", "0x10", "+3", "1.5", "-2"}) {
    EXPECT_TRUE(ParseScore(ok, &d)) << ok;
  }
  EXPECT_TRUE(ParseScore("-0", &d));
  EXPECT_TRUE(std::signbit(d));  // parsed as -0.0; the zset layer normalises it (F2)
  for (const char* bad : {"nan", "-nan", "", " 1", "1 ", "1e999", "1e-999", "1_0", "abc"}) {
    EXPECT_FALSE(ParseScore(bad, &d)) << bad;
  }
  EXPECT_FALSE(ParseScore(std::string_view("1\0", 2), &d));  // embedded NUL: not all bytes consumed
}

TEST(NormalizeScore, MinusZeroBecomesZero) {
  EXPECT_FALSE(std::signbit(NormalizeScore(-0.0)));
  EXPECT_EQ(NormalizeScore(-1.5), -1.5);
}

}  // namespace
}  // namespace tideskv
