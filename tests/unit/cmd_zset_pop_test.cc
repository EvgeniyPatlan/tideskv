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

struct ZsetPopCmds : ::testing::Test {
  void SetUp() override { ASSERT_EQ(h.Run({"ZADD", "z", "1", "a", "2", "b", "3", "c"}), Int(3)); }
  CommandHarness h;
};

TEST_F(ZsetPopCmds, RemRangeBy) {
  EXPECT_EQ(h.Run({"ZREMRANGEBYRANK", "z", "0", "0"}), Int(1));
  EXPECT_EQ(h.Run({"ZREMRANGEBYSCORE", "z", "(2", "+inf"}), Int(1));
  EXPECT_EQ(h.Run({"ZREMRANGEBYLEX", "z", "-", "+"}), Int(1));
  EXPECT_EQ(h.Run({"EXISTS", "z"}), Int(0));
  EXPECT_EQ(h.Run({"ZREMRANGEBYRANK", "nokey", "0", "-1"}), Int(0));
  EXPECT_EQ(h.Run({"ZREMRANGEBYRANK", "z", "x", "1"}), Err(kErrNotInteger));
  EXPECT_EQ(h.Run({"ZREMRANGEBYSCORE", "z", "x", "1"}), Err(kErrMinMaxNotFloat));
  EXPECT_EQ(h.Run({"ZREMRANGEBYLEX", "z", "x", "+"}), Err(kErrMinMaxNotString));
}

TEST_F(ZsetPopCmds, PopShapesResp2) {
  EXPECT_EQ(h.Run({"ZPOPMIN", "z"}), Arr({Bulk("a"), Bulk("1")}));
  EXPECT_EQ(h.Run({"ZPOPMAX", "z", "1"}), Arr({Bulk("c"), Bulk("3")}));  // still flat in RESP2
  EXPECT_EQ(h.Run({"ZPOPMIN", "z", "0"}), "*0\r\n");
  EXPECT_EQ(h.Run({"ZPOPMIN", "nokey"}), "*0\r\n");
  EXPECT_EQ(h.Run({"ZPOPMIN", "z", "-1"}), Err(kErrMustBePositive));
  EXPECT_EQ(h.Run({"ZPOPMIN", "z", "abc"}), Err(kErrMustBePositive));
  EXPECT_EQ(h.Run({"ZPOPMIN", "z", "1", "2"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"SET", "s", "v"}), kOk);
  EXPECT_EQ(h.Run({"ZPOPMIN", "s", "0"}), Err(kErrWrongType));  // type before the count-0 shortcut
}

TEST_F(ZsetPopCmds, PopShapesResp3) {
  h.Resp3();
  EXPECT_EQ(h.Run({"ZPOPMIN", "z"}), "*2\r\n" + Bulk("a") + ",1\r\n");
  EXPECT_EQ(h.Run({"ZPOPMIN", "z", "1"}), "*1\r\n*2\r\n" + Bulk("b") + ",2\r\n");
}

TEST_F(ZsetPopCmds, Zmpop) {
  EXPECT_EQ(h.Run({"ZMPOP", "2", "nokey", "z", "MAX", "COUNT", "2"}),
            Arr({Bulk("z"), Arr({Arr({Bulk("c"), Bulk("3")}), Arr({Bulk("b"), Bulk("2")})})}));
  EXPECT_EQ(h.Run({"ZMPOP", "1", "nokey", "MIN"}), "*-1\r\n");
  EXPECT_EQ(h.Run({"ZMPOP", "0", "z", "MIN"}), Err(kErrNumkeysNotPositive));
  EXPECT_EQ(h.Run({"ZMPOP", "1", "z", "MIN", "COUNT", "0"}), Err(kErrCountGreaterThanZero));
  EXPECT_EQ(h.Run({"ZMPOP", "1", "z", "LEFT"}), Err(kErrSyntax));
}

TEST_F(ZsetPopCmds, Zrandmember) {
  EXPECT_EQ(h.Run({"ZRANDMEMBER", "z", "10"}), Arr({Bulk("c"), Bulk("b"), Bulk("a")}));
  EXPECT_EQ(h.Run({"ZRANDMEMBER", "z", "10", "WITHSCORES"}),
            Arr({Bulk("c"), Bulk("3"), Bulk("b"), Bulk("2"), Bulk("a"), Bulk("1")}));
  EXPECT_EQ(h.Run({"ZRANDMEMBER", "nokey"}), kNil);
  EXPECT_EQ(h.Run({"ZRANDMEMBER", "nokey", "3"}), "*0\r\n");
  EXPECT_EQ(h.Run({"ZRANDMEMBER", "z", "0"}), "*0\r\n");
  EXPECT_EQ(h.Run({"ZRANDMEMBER", "z", "x", "WITHSCORES"}), Err(kErrNotInteger));
  EXPECT_EQ(h.Run({"ZRANDMEMBER", "z", "1", "BOGUS"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"ZRANDMEMBER", "z", "-9223372036854775807", "WITHSCORES"}), Err(kErrValueOutOfRange));
  EXPECT_EQ(h.Run({"ZRANDMEMBER", "z", "-9223372036854775808"}), Err(kErrCountOutOfRange));
}

}  // namespace
}  // namespace tideskv
