#include <gtest/gtest.h>

#include "support/command_harness.h"
#include "types/errors.h"

namespace tideskv {
namespace {

using test::Arr;
using test::Bulk;
using test::CommandHarness;
using test::Err;
using test::Int;
using test::kNil;
using test::kOk;

struct ZsetCmds : ::testing::Test {
  CommandHarness h;
};

TEST_F(ZsetCmds, AddScoreCardRem) {
  EXPECT_EQ(h.Run({"ZADD", "z", "1", "a", "2", "b"}), Int(2));
  EXPECT_EQ(h.Run({"ZADD", "z", "CH", "3", "a", "2", "b", "4", "c"}), Int(2));  // c added, a changed
  EXPECT_EQ(h.Run({"ZSCORE", "z", "a"}), Bulk("3"));
  EXPECT_EQ(h.Run({"ZSCORE", "z", "zz"}), kNil);
  EXPECT_EQ(h.Run({"ZSCORE", "nokey", "a"}), kNil);
  EXPECT_EQ(h.Run({"ZMSCORE", "z", "a", "zz", "c"}), Arr({Bulk("3"), kNil, Bulk("4")}));
  EXPECT_EQ(h.Run({"ZMSCORE", "nokey", "a", "b"}), Arr({kNil, kNil}));
  EXPECT_EQ(h.Run({"ZCARD", "z"}), Int(3));
  EXPECT_EQ(h.Run({"ZCARD", "nokey"}), Int(0));
  EXPECT_EQ(h.Run({"ZREM", "z", "a", "zz"}), Int(1));
  EXPECT_EQ(h.Run({"ZREM", "nokey", "a"}), Int(0));
  EXPECT_EQ(h.Run({"TYPE", "z"}), "+zset\r\n");
}

TEST_F(ZsetCmds, IncrForms) {
  EXPECT_EQ(h.Run({"ZINCRBY", "z", "2", "a"}), Bulk("2"));
  EXPECT_EQ(h.Run({"ZINCRBY", "z", "0.1", "a"}), Bulk("2.1"));
  EXPECT_EQ(h.Run({"ZADD", "z", "INCR", "1", "a"}), Bulk("3.1"));
  EXPECT_EQ(h.Run({"ZADD", "z", "NX", "INCR", "1", "a"}), kNil);
  EXPECT_EQ(h.Run({"ZADD", "z", "XX", "INCR", "1", "zz"}), kNil);
  EXPECT_EQ(h.Run({"ZINCRBY", "z", "nx", "a"}), Err(kErrSyntax));  // the increment is read as an option
  h.Resp3();
  EXPECT_EQ(h.Run({"ZINCRBY", "z", "1", "a"}), ",4.1\r\n");
  EXPECT_EQ(h.Run({"ZSCORE", "z", "zz"}), "_\r\n");
}

TEST_F(ZsetCmds, ZaddErrorsInRedisOrder) {
  EXPECT_EQ(h.Run({"ZADD", "z", "1"}), "-ERR wrong number of arguments for 'zadd' command\r\n");  // arity -4
  EXPECT_EQ(h.Run({"ZADD", "z", "1", "a", "2"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"ZADD", "z", "NX", "XX", "1", "a"}), Err(kErrZaddXxNx));
  EXPECT_EQ(h.Run({"ZADD", "z", "GT", "NX", "1", "a"}), Err(kErrZaddGtLtNx));
  EXPECT_EQ(h.Run({"ZADD", "z", "GT", "LT", "1", "a"}), Err(kErrZaddGtLtNx));
  EXPECT_EQ(h.Run({"ZADD", "z", "INCR", "1", "a", "2", "b"}), Err(kErrZaddIncrPair));
  EXPECT_EQ(h.Run({"ZADD", "z", "nan", "a"}), Err(kErrNotFloat));
  EXPECT_EQ(h.Run({"ZADD", "z", "", "a"}), Err(kErrNotFloat));
  EXPECT_EQ(h.Run({"SET", "s", "v"}), kOk);
  EXPECT_EQ(h.Run({"ZADD", "s", "x", "a"}), Err(kErrNotFloat));  // scores before the type check
  EXPECT_EQ(h.Run({"ZADD", "s", "1", "a"}), Err(kErrWrongType));
  EXPECT_EQ(h.Run({"ZADD", "z", "inf", "a"}), Int(1));
  EXPECT_EQ(h.Run({"ZINCRBY", "z", "-inf", "a"}), Err(kErrScoreNan));
}

TEST_F(ZsetCmds, ScoreFormatting) {  // Review Focus #5
  EXPECT_EQ(h.Run({"ZADD", "z", "1e20", "a", "-0", "b", "0.1", "c", "-inf", "d"}), Int(4));
  EXPECT_EQ(h.Run({"ZSCORE", "z", "a"}), Bulk("1e+20"));
  EXPECT_EQ(h.Run({"ZSCORE", "z", "b"}), Bulk("0"));  // -0 is stored as 0 (F2)
  EXPECT_EQ(h.Run({"ZSCORE", "z", "c"}), Bulk("0.1"));
  EXPECT_EQ(h.Run({"ZSCORE", "z", "d"}), Bulk("-inf"));
}

TEST_F(ZsetCmds, CountAndLexCount) {
  EXPECT_EQ(h.Run({"ZADD", "z", "1", "a", "2", "b", "3", "c"}), Int(3));
  EXPECT_EQ(h.Run({"ZCOUNT", "z", "(1", "+inf"}), Int(2));
  EXPECT_EQ(h.Run({"ZCOUNT", "nokey", "0", "1"}), Int(0));
  EXPECT_EQ(h.Run({"ZCOUNT", "z", "a", "1"}), Err(kErrMinMaxNotFloat));
  EXPECT_EQ(h.Run({"ZADD", "l", "0", "a", "0", "b", "0", "c"}), Int(3));
  EXPECT_EQ(h.Run({"ZLEXCOUNT", "l", "[b", "+"}), Int(2));
  EXPECT_EQ(h.Run({"ZLEXCOUNT", "l", "+", "-"}), Int(0));
  EXPECT_EQ(h.Run({"ZLEXCOUNT", "l", "b", "+"}), Err(kErrMinMaxNotString));
  EXPECT_EQ(h.Run({"SET", "s", "v"}), kOk);
  EXPECT_EQ(h.Run({"ZCOUNT", "s", "a", "1"}), Err(kErrMinMaxNotFloat));  // range before the type
  EXPECT_EQ(h.Run({"ZCOUNT", "s", "0", "1"}), Err(kErrWrongType));
}

TEST_F(ZsetCmds, LastMemberDeletesTheKeyAndAddKeepsTtl) {
  EXPECT_EQ(h.Run({"ZADD", "z", "1", "a"}), Int(1));
  EXPECT_EQ(h.Run({"EXPIRE", "z", "100"}), Int(1));
  EXPECT_EQ(h.Run({"ZADD", "z", "2", "b"}), Int(1));
  EXPECT_EQ(h.Run({"TTL", "z"}), Int(100));
  EXPECT_EQ(h.Run({"ZREM", "z", "a", "b"}), Int(2));
  EXPECT_TRUE(h.Cf(ColumnFamily::kMeta).empty());
  EXPECT_TRUE(h.Cf(ColumnFamily::kExpire).empty());
  EXPECT_TRUE(h.Cf(ColumnFamily::kZScore).empty());
}

TEST_F(ZsetCmds, ExpiredButStoredZsetIsMissing) {
  EXPECT_EQ(h.Run({"ZADD", "z", "1", "old"}), Int(1));
  EXPECT_EQ(h.Run({"PEXPIRE", "z", "5"}), Int(1));
  h.clock().Advance(5);
  EXPECT_EQ(h.Run({"ZCARD", "z"}), Int(0));
  EXPECT_EQ(h.Run({"ZADD", "z", "XX", "1", "old"}), Int(0));
  EXPECT_EQ(h.Run({"ZADD", "z", "2", "new"}), Int(1));
  EXPECT_EQ(h.Run({"ZSCORE", "z", "old"}), kNil);
}

}  // namespace
}  // namespace tideskv
