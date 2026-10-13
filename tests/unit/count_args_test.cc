#include "commands/count_args.h"

#include <gtest/gtest.h>

#include "types/errors.h"

namespace tideskv {
namespace {

TEST(CountArgs, RandomCount) {
  int64_t n = 0;
  EXPECT_TRUE(ParseRandomCount("-5", &n).ok());
  EXPECT_EQ(n, -5);
  EXPECT_TRUE(ParseRandomCount("-9223372036854775807", &n).ok());
  EXPECT_EQ(ParseRandomCount("-9223372036854775808", &n).message(), kErrCountOutOfRange);
  EXPECT_EQ(ParseRandomCount("x", &n).message(), kErrNotInteger);
  EXPECT_EQ(ParseRandomCount("1.0", &n).message(), kErrNotInteger);
}

TEST(CountArgs, PositiveCountUsesTheGivenMessageForEveryFailure) {
  int64_t n = 0;
  EXPECT_TRUE(ParsePositiveCount("0", kErrMustBePositive, &n).ok());
  EXPECT_EQ(ParsePositiveCount("-1", kErrMustBePositive, &n).message(), kErrMustBePositive);
  EXPECT_EQ(ParsePositiveCount("abc", kErrMustBePositive, &n).message(), kErrMustBePositive);
  EXPECT_EQ(ParsePositiveCount("-1", kErrLimitNegative, &n).message(), kErrLimitNegative);
}

TEST(CountArgs, NonNegativeCount) {
  int64_t n = -1;
  EXPECT_TRUE(ParseNonNegativeCount("0", &n).ok());
  EXPECT_EQ(n, 0);
  EXPECT_EQ(ParseNonNegativeCount("x", &n).message(), kErrNotInteger);
  EXPECT_EQ(ParseNonNegativeCount("-1", &n).message(), kErrMustBePositive);
}

}  // namespace
}  // namespace tideskv
