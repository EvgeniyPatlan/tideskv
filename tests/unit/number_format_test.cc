#include "common/number_format.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace tideskv {
namespace {

long double P(std::string_view s) {
  long double v = 0;
  EXPECT_TRUE(ParseLongDouble(s, &v)) << s;
  return v;
}

TEST(NumberFormat, ParsesWhatRedisParses) {
  EXPECT_EQ(P("10.5"), 10.5L);
  EXPECT_EQ(P("-3"), -3.0L);
  EXPECT_EQ(P("5.0e3"), 5000.0L);
  EXPECT_EQ(P("0x10"), 16.0L);  // strtold accepts hex, as in Redis
  long double v = 0;
  EXPECT_TRUE(ParseLongDouble("inf", &v));
}

TEST(NumberFormat, RejectsWhatRedisRejects) {
  long double v = 0;
  for (std::string_view bad : {"", " 1", "1 ", "abc", "1.5x", "nan", "-nan", "1e999999"}) {
    EXPECT_FALSE(ParseLongDouble(bad, &v)) << bad;
  }
  EXPECT_FALSE(ParseLongDouble(std::string_view("1\0", 2), &v));
  EXPECT_FALSE(ParseLongDouble(std::string(5120, '1'), &v));
}

TEST(NumberFormat, HumanFormat) {
  EXPECT_EQ(FormatLongDouble(10.5L), "10.5");
  EXPECT_EQ(FormatLongDouble(3.0L), "3");
  EXPECT_EQ(FormatLongDouble(5000.0L), "5000");
  EXPECT_EQ(FormatLongDouble(-0.0L), "0");
  EXPECT_EQ(FormatLongDouble(P("0.1") + P("0.2")), "0.3");
  EXPECT_EQ(FormatLongDouble(P("10.5") + P("0.1")), "10.6");
  EXPECT_EQ(FormatLongDouble(-1.25L), "-1.25");
}

TEST(NumberFormat, RoundTripsThroughIncrByFloatStyleUse) {
  long double acc = 0;
  for (int i = 0; i < 10; ++i) acc = P(FormatLongDouble(acc + P("0.1")));
  EXPECT_EQ(FormatLongDouble(acc), "1");
}

}  // namespace
}  // namespace tideskv
