#include <gtest/gtest.h>

#include "commands/reply_util.h"
#include "support/command_harness.h"
#include "types/errors.h"

namespace tideskv {
namespace {

using test::Arr;
using test::Bulk;
using test::CommandHarness;
using test::Err;
using test::Int;
using test::kOk;

struct ZsetAlgebraCmds : ::testing::Test {
  void SetUp() override {
    ASSERT_EQ(h.Run({"ZADD", "{t}a", "1", "x", "2", "y"}), Int(2));
    ASSERT_EQ(h.Run({"ZADD", "{t}b", "3", "y", "4", "z"}), Int(2));
    ASSERT_EQ(h.Run({"SADD", "{t}s", "x", "z"}), Int(2));
  }
  CommandHarness h;
};

TEST_F(ZsetAlgebraCmds, UnionInterDiff) {
  EXPECT_EQ(h.Run({"ZUNION", "2", "{t}a", "{t}b", "WITHSCORES"}),
            Arr({Bulk("x"), Bulk("1"), Bulk("z"), Bulk("4"), Bulk("y"), Bulk("5")}));
  EXPECT_EQ(h.Run({"ZINTER", "2", "{t}a", "{t}b", "AGGREGATE", "MAX"}), Arr({Bulk("y")}));
  EXPECT_EQ(h.Run({"ZDIFF", "2", "{t}a", "{t}b", "WITHSCORES"}), Arr({Bulk("x"), Bulk("1")}));
  EXPECT_EQ(
      h.Run({"ZUNION", "2", "{t}a", "{t}s", "WEIGHTS", "2", "10", "WITHSCORES"}),
      Arr({Bulk("y"), Bulk("4"), Bulk("z"), Bulk("10"), Bulk("x"), Bulk("12")}));  // set members score 1
  EXPECT_EQ(h.Run({"ZINTERCARD", "2", "{t}a", "{t}b"}), Int(1));
  EXPECT_EQ(h.Run({"ZINTERCARD", "2", "{t}a", "{t}s", "LIMIT", "0"}), Int(1));
}

TEST_F(ZsetAlgebraCmds, StoreVariants) {
  EXPECT_EQ(h.Run({"SET", "{t}d", "v"}), kOk);
  EXPECT_EQ(h.Run({"ZUNIONSTORE", "{t}d", "2", "{t}a", "{t}b"}), Int(3));
  EXPECT_EQ(h.Run({"ZRANGE", "{t}d", "0", "-1", "WITHSCORES"}),
            Arr({Bulk("x"), Bulk("1"), Bulk("z"), Bulk("4"), Bulk("y"), Bulk("5")}));
  EXPECT_EQ(h.Run({"ZINTERSTORE", "{t}d", "2", "{t}a", "{t}nokey"}), Int(0));
  EXPECT_EQ(h.Run({"EXISTS", "{t}d"}), Int(0));
  EXPECT_EQ(h.Run({"ZDIFFSTORE", "{t}a", "2", "{t}a", "{t}b"}), Int(1));  // dst is also a source
  EXPECT_EQ(h.Run({"ZRANGE", "{t}a", "0", "-1"}), Arr({Bulk("x")}));
  EXPECT_EQ(h.Run({"ZUNIONSTORE", "{t}d", "1", "{t}a", "WITHSCORES"}), Err(kErrSyntax));
}

TEST_F(ZsetAlgebraCmds, ErrorsInRedisOrder) {
  EXPECT_EQ(h.Run({"ZUNIONSTORE", "{t}d", "0", "{t}a"}), Err(AtLeastOneKeyError("zunionstore")));
  EXPECT_EQ(h.Run({"ZINTERCARD", "0", "{t}a"}), Err(AtLeastOneKeyError("zintercard")));
  EXPECT_EQ(h.Run({"ZUNION", "x", "{t}a"}), Err(kErrNotInteger));
  EXPECT_EQ(h.Run({"ZUNION", "3", "{t}a", "{t}b"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"ZUNION", "2", "{t}a", "{t}b", "WEIGHTS", "1", "x"}), Err(kErrWeightNotFloat));
  EXPECT_EQ(h.Run({"ZUNION", "2", "{t}a", "{t}b", "WEIGHTS", "1"}), Err(kErrSyntax));  // too few: not matched
  EXPECT_EQ(h.Run({"ZUNION", "2", "{t}a", "{t}b", "AGGREGATE", "avg"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"ZDIFF", "2", "{t}a", "{t}b", "WEIGHTS", "1", "1"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"ZINTERCARD", "1", "{t}a", "LIMIT", "-1"}), Err(kErrLimitNegative));
  EXPECT_EQ(h.Run({"ZINTERCARD", "1", "{t}a", "WITHSCORES"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"SET", "{t}str", "v"}), kOk);
  EXPECT_EQ(h.Run({"ZUNION", "2", "{t}a", "{t}str", "WEIGHTS", "x", "y"}), Err(kErrWrongType));  // type first
}

TEST_F(ZsetAlgebraCmds, Resp3WithScores) {
  h.Resp3();
  EXPECT_EQ(h.Run({"ZDIFF", "2", "{t}a", "{t}b", "WITHSCORES"}), "*1\r\n*2\r\n" + Bulk("x") + ",1\r\n");
}

}  // namespace
}  // namespace tideskv
