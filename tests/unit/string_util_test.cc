#include "common/string_util.h"

#include <gtest/gtest.h>

#include <limits>

namespace tideskv {
namespace {

TEST(ParseInt64, AcceptsStrictDecimal) {
  int64_t v = 0;
  EXPECT_TRUE(ParseInt64("0", &v));
  EXPECT_EQ(v, 0);
  EXPECT_TRUE(ParseInt64("-42", &v));
  EXPECT_EQ(v, -42);
  EXPECT_TRUE(ParseInt64("9223372036854775807", &v));
  EXPECT_EQ(v, std::numeric_limits<int64_t>::max());
}

TEST(ParseInt64, RejectsGarbage) {
  int64_t v = 0;
  for (const char* bad : {"", "-", "+1", " 1", "1 ", "1a", "0x10", "9223372036854775808", "1.0"}) {
    EXPECT_FALSE(ParseInt64(bad, &v)) << bad;
  }
}

TEST(ParseUint64, RejectsNegative) {
  uint64_t v = 0;
  EXPECT_FALSE(ParseUint64("-1", &v));
  EXPECT_TRUE(ParseUint64("18446744073709551615", &v));
}

TEST(ParseMemory, RedisUnits) {
  uint64_t v = 0;
  EXPECT_TRUE(ParseMemory("100", &v));
  EXPECT_EQ(v, 100u);
  EXPECT_TRUE(ParseMemory("1k", &v));
  EXPECT_EQ(v, 1000u);
  EXPECT_TRUE(ParseMemory("1kb", &v));
  EXPECT_EQ(v, 1024u);
  EXPECT_TRUE(ParseMemory("64MB", &v));
  EXPECT_EQ(v, 64ULL << 20);
  EXPECT_TRUE(ParseMemory("4gb", &v));
  EXPECT_EQ(v, 4ULL << 30);
  EXPECT_TRUE(ParseMemory("2g", &v));
  EXPECT_EQ(v, 2000000000u);
}

TEST(ParseMemory, RejectsBadInput) {
  uint64_t v = 0;
  for (const char* bad : {"", "gb", "1tb", "-1k", "1 kb", "99999999999999999999gb"}) {
    EXPECT_FALSE(ParseMemory(bad, &v)) << bad;
  }
}

TEST(StringUtil, CaseHelpers) {
  EXPECT_EQ(ToLower("PiNg"), "ping");
  EXPECT_TRUE(EqualsIgnoreCase("CONFIG", "config"));
  EXPECT_FALSE(EqualsIgnoreCase("config", "configs"));
}

TEST(StringUtil, ConstantTimeEquals) {
  EXPECT_TRUE(ConstantTimeEquals("secret", "secret"));
  EXPECT_FALSE(ConstantTimeEquals("secret", "secreT"));
  EXPECT_FALSE(ConstantTimeEquals("secret", "secret2"));
  EXPECT_FALSE(ConstantTimeEquals("", "x"));
  EXPECT_TRUE(ConstantTimeEquals("", ""));
}

}  // namespace
}  // namespace tideskv
