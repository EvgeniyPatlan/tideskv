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

struct ListMoveCmds : ::testing::Test {
  void SetUp() override { ASSERT_EQ(h.Run({"RPUSH", "a", "1", "2", "3"}), Int(3)); }
  CommandHarness h;
};

TEST_F(ListMoveCmds, LmoveAndRpoplpush) {
  EXPECT_EQ(h.Run({"LMOVE", "a", "b", "left", "RIGHT"}), Bulk("1"));
  EXPECT_EQ(h.Run({"RPOPLPUSH", "a", "b"}), Bulk("3"));
  EXPECT_EQ(h.Run({"LRANGE", "b", "0", "-1"}), Arr({Bulk("3"), Bulk("1")}));
  EXPECT_EQ(h.Run({"LMOVE", "nokey", "b", "left", "left"}), kNil);
  EXPECT_EQ(h.Run({"LMOVE", "a", "b", "up", "left"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"SET", "s", "v"}), kOk);
  EXPECT_EQ(h.Run({"LMOVE", "a", "s", "left", "left"}), Err(kErrWrongType));
  EXPECT_EQ(h.Run({"LRANGE", "a", "0", "-1"}), Arr({Bulk("2")}));  // src untouched
  EXPECT_EQ(h.Run({"LMOVE", "s", "b", "left", "left"}), Err(kErrWrongType));
  EXPECT_EQ(h.Run({"LMOVE", "nokey", "s", "left", "left"}), kNil);  // dst never looked at
}

TEST_F(ListMoveCmds, Lmpop) {
  EXPECT_EQ(h.Run({"LMPOP", "2", "nokey", "a", "RIGHT", "COUNT", "2"}),
            Arr({Bulk("a"), Arr({Bulk("3"), Bulk("2")})}));
  EXPECT_EQ(h.Run({"LMPOP", "1", "a", "left"}), Arr({Bulk("a"), Arr({Bulk("1")})}));
  EXPECT_EQ(h.Run({"LMPOP", "1", "a", "left"}), "*-1\r\n");
  EXPECT_EQ(h.Run({"LMPOP", "0", "a", "left"}), Err(kErrNumkeysNotPositive));
  EXPECT_EQ(h.Run({"LMPOP", "1", "a", "left", "COUNT", "0"}), Err(kErrCountGreaterThanZero));
  h.Resp3();
  EXPECT_EQ(h.Run({"LMPOP", "1", "a", "left"}), "_\r\n");
}

TEST_F(ListMoveCmds, LmpopWrongTypeBeforeAList) {
  EXPECT_EQ(h.Run({"SET", "s", "v"}), kOk);
  EXPECT_EQ(h.Run({"LMPOP", "2", "s", "a", "left"}), Err(kErrWrongType));
  EXPECT_EQ(h.Run({"LMPOP", "2", "a", "s", "left"}), Arr({Bulk("a"), Arr({Bulk("1")})}));
}

}  // namespace
}  // namespace tideskv
