#include "commands/numkeys_args.h"

#include <gtest/gtest.h>

#include "types/errors.h"

namespace tideskv {
namespace {

Status Parse(const ArgList& argv, MultiPopArgs* out) { return ParseMultiPop(argv, 1, "LEFT", "RIGHT", out); }

TEST(NumkeysArgs, ParsesKeysWhereAndCount) {
  MultiPopArgs a;
  ASSERT_TRUE(Parse({"lmpop", "2", "k1", "k2", "right", "COUNT", "3"}, &a).ok());
  EXPECT_EQ(a.first_key, 2u);
  EXPECT_EQ(a.num_keys, 2u);
  EXPECT_TRUE(a.second);
  EXPECT_EQ(a.count, 3);
  EXPECT_TRUE(a.has_count);
  ASSERT_TRUE(Parse({"lmpop", "1", "k", "left"}, &a).ok());
  EXPECT_FALSE(a.second);
  EXPECT_EQ(a.count, 1);
  EXPECT_FALSE(a.has_count);
}

TEST(NumkeysArgs, RedisErrorTexts) {
  MultiPopArgs a;
  EXPECT_EQ(Parse({"lmpop", "0", "k", "left"}, &a).message(), kErrNumkeysNotPositive);
  EXPECT_EQ(Parse({"lmpop", "x", "k", "left"}, &a).message(), kErrNumkeysNotPositive);
  EXPECT_EQ(Parse({"lmpop", "2", "k", "left"}, &a).message(), kErrSyntax);  // "left" taken as a key
  EXPECT_EQ(Parse({"lmpop", "9223372036854775807", "k", "left"}, &a).message(), kErrSyntax);
  EXPECT_EQ(Parse({"lmpop", "1", "k", "up"}, &a).message(), kErrSyntax);
  EXPECT_EQ(Parse({"lmpop", "1", "k", "left", "COUNT"}, &a).message(), kErrSyntax);
  EXPECT_EQ(Parse({"lmpop", "1", "k", "left", "COUNT", "0"}, &a).message(), kErrCountGreaterThanZero);
  EXPECT_EQ(Parse({"lmpop", "1", "k", "left", "COUNT", "x"}, &a).message(), kErrCountGreaterThanZero);
  EXPECT_EQ(Parse({"lmpop", "1", "k", "left", "COUNT", "1", "COUNT", "2"}, &a).message(), kErrSyntax);
  EXPECT_EQ(Parse({"lmpop", "1", "k", "left", "junk"}, &a).message(), kErrSyntax);
}

}  // namespace
}  // namespace tideskv
