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

struct ListEditCmds : ::testing::Test {
  void SetUp() override { ASSERT_EQ(h.Run({"RPUSH", "l", "a", "b", "c", "b"}), Int(4)); }
  CommandHarness h;
};

TEST_F(ListEditCmds, Linsert) {
  EXPECT_EQ(h.Run({"LINSERT", "l", "before", "b", "x"}), Int(5));
  EXPECT_EQ(h.Run({"LINSERT", "l", "AFTER", "c", "y"}), Int(6));
  EXPECT_EQ(h.Run({"LRANGE", "l", "0", "-1"}),
            Arr({Bulk("a"), Bulk("x"), Bulk("b"), Bulk("c"), Bulk("y"), Bulk("b")}));
  EXPECT_EQ(h.Run({"LINSERT", "l", "after", "zz", "q"}), Int(-1));
  EXPECT_EQ(h.Run({"LINSERT", "nokey", "after", "a", "q"}), Int(0));
  EXPECT_EQ(h.Run({"LINSERT", "l", "middle", "a", "q"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"SET", "s", "v"}), kOk);
  EXPECT_EQ(h.Run({"LINSERT", "s", "middle", "a", "q"}), Err(kErrSyntax));  // the token is checked first
  EXPECT_EQ(h.Run({"LINSERT", "s", "after", "a", "q"}), Err(kErrWrongType));
}

TEST_F(ListEditCmds, Lrem) {
  EXPECT_EQ(h.Run({"LREM", "l", "-1", "b"}), Int(1));
  EXPECT_EQ(h.Run({"LRANGE", "l", "0", "-1"}), Arr({Bulk("a"), Bulk("b"), Bulk("c")}));
  EXPECT_EQ(h.Run({"LREM", "l", "0", "zz"}), Int(0));
  EXPECT_EQ(h.Run({"LREM", "nokey", "0", "zz"}), Int(0));
  EXPECT_EQ(h.Run({"LREM", "l", "x", "a"}), Err(kErrNotInteger));
}

TEST_F(ListEditCmds, Ltrim) {
  EXPECT_EQ(h.Run({"LTRIM", "l", "1", "-1"}), kOk);
  EXPECT_EQ(h.Run({"LRANGE", "l", "0", "-1"}), Arr({Bulk("b"), Bulk("c"), Bulk("b")}));
  EXPECT_EQ(h.Run({"LTRIM", "nokey", "0", "1"}), kOk);
  EXPECT_EQ(h.Run({"LTRIM", "l", "x", "1"}), Err(kErrNotInteger));
  EXPECT_EQ(h.Run({"LTRIM", "l", "2", "1"}), kOk);
  EXPECT_EQ(h.Run({"EXISTS", "l"}), Int(0));
}

TEST_F(ListEditCmds, Lpos) {
  EXPECT_EQ(h.Run({"LPOS", "l", "b"}), Int(1));
  EXPECT_EQ(h.Run({"LPOS", "l", "b", "RANK", "-1"}), Int(3));
  EXPECT_EQ(h.Run({"LPOS", "l", "b", "COUNT", "0"}), Arr({Int(1), Int(3)}));
  EXPECT_EQ(h.Run({"LPOS", "l", "b", "RANK", "-1", "COUNT", "0"}), Arr({Int(3), Int(1)}));
  EXPECT_EQ(h.Run({"LPOS", "l", "b", "MAXLEN", "1"}), kNil);
  EXPECT_EQ(h.Run({"LPOS", "l", "zz"}), kNil);
  EXPECT_EQ(h.Run({"LPOS", "l", "zz", "COUNT", "2"}), "*0\r\n");
  EXPECT_EQ(h.Run({"LPOS", "nokey", "a"}), kNil);
  EXPECT_EQ(h.Run({"LPOS", "nokey", "a", "COUNT", "1"}), "*0\r\n");
}

TEST_F(ListEditCmds, LposErrors) {  // options are parsed before the key lookup
  EXPECT_EQ(h.Run({"LPOS", "nokey", "a", "RANK", "0"}), Err(kErrRankZero));
  EXPECT_EQ(h.Run({"LPOS", "nokey", "a", "RANK", "x"}), Err(kErrNotInteger));
  EXPECT_EQ(h.Run({"LPOS", "nokey", "a", "RANK", "-9223372036854775808"}), Err(kErrCountOutOfRange));
  EXPECT_EQ(h.Run({"LPOS", "nokey", "a", "COUNT", "-1"}), Err(kErrCountNegative));
  EXPECT_EQ(h.Run({"LPOS", "nokey", "a", "COUNT", "x"}), Err(kErrCountNegative));
  EXPECT_EQ(h.Run({"LPOS", "nokey", "a", "MAXLEN", "x"}), Err(kErrMaxlenNegative));
  EXPECT_EQ(h.Run({"LPOS", "nokey", "a", "RANK"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"LPOS", "nokey", "a", "BOGUS", "1"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"LPOS", "l", "b", "rank", "1", "RANK", "2"}), Int(3));  // repeats: last wins
}

}  // namespace
}  // namespace tideskv
