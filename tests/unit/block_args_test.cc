#include "commands/block_args.h"

#include <gtest/gtest.h>

#include <cstdint>

#include "types/errors.h"

namespace tideskv {
namespace {

constexpr int64_t kNow = 1'700'000'000'000;

int64_t Ok(std::string_view arg) {
  int64_t ms = -1;
  const Status st = ParseBlockTimeout(arg, kNow, &ms);
  EXPECT_TRUE(st.ok()) << arg << ": " << st.ToString();
  return ms;
}

std::string Err(std::string_view arg) {
  int64_t ms = -1;
  return ParseBlockTimeout(arg, kNow, &ms).message();
}

TEST(BlockArgs, SecondsToMillisecondsRoundedUp) {
  EXPECT_EQ(Ok("0"), 0);
  EXPECT_EQ(Ok("1"), 1000);
  EXPECT_EQ(Ok("0.2"), 200);
  EXPECT_EQ(Ok("0.0001"), 1);  // ceil
  EXPECT_EQ(Ok(".5"), 500);
  EXPECT_EQ(Ok("1e1"), 10000);
  EXPECT_EQ(Ok("-0.0001"), 0);  // ceil(-0.1) is 0: blocks forever, as in Redis
}

TEST(BlockArgs, RedisErrorTexts) {
  EXPECT_EQ(Err("x"), kErrTimeoutNotFloat);
  EXPECT_EQ(Err(""), kErrTimeoutNotFloat);
  EXPECT_EQ(Err(" 1"), kErrTimeoutNotFloat);
  EXPECT_EQ(Err("nan"), kErrTimeoutNotFloat);
  EXPECT_EQ(Err("-1"), kErrTimeoutNegative);
  EXPECT_EQ(Err("-inf"), kErrTimeoutNegative);
  EXPECT_EQ(Err("inf"), kErrTimeoutOutOfRange);
  EXPECT_EQ(Err("9223372036854775"), kErrTimeoutOutOfRange);  // ms + now overflows
}

}  // namespace
}  // namespace tideskv
