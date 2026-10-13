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

struct ListCmds : ::testing::Test {
  CommandHarness h;
};

TEST_F(ListCmds, PushLenRangeIndex) {
  EXPECT_EQ(h.Run({"RPUSH", "l", "a", "b"}), Int(2));
  EXPECT_EQ(h.Run({"LPUSH", "l", "x", "y"}), Int(4));
  EXPECT_EQ(h.Run({"LRANGE", "l", "0", "-1"}), Arr({Bulk("y"), Bulk("x"), Bulk("a"), Bulk("b")}));
  EXPECT_EQ(h.Run({"LLEN", "l"}), Int(4));
  EXPECT_EQ(h.Run({"LLEN", "nokey"}), Int(0));
  EXPECT_EQ(h.Run({"LINDEX", "l", "-1"}), Bulk("b"));
  EXPECT_EQ(h.Run({"LINDEX", "l", "9"}), kNil);
  EXPECT_EQ(h.Run({"LINDEX", "nokey", "notanumber"}), kNil);
  EXPECT_EQ(h.Run({"LINDEX", "l", "notanumber"}), Err(kErrNotInteger));
  EXPECT_EQ(h.Run({"LRANGE", "nokey", "0", "-1"}), "*0\r\n");
  EXPECT_EQ(h.Run({"TYPE", "l"}), "+list\r\n");
}

TEST_F(ListCmds, PushxOnlyOnExistingLists) {
  EXPECT_EQ(h.Run({"LPUSHX", "l", "a"}), Int(0));
  EXPECT_EQ(h.Run({"EXISTS", "l"}), Int(0));
  EXPECT_EQ(h.Run({"RPUSH", "l", "a"}), Int(1));
  EXPECT_EQ(h.Run({"RPUSHX", "l", "b", "c"}), Int(3));
  EXPECT_EQ(h.Run({"LPUSHX", "l", "z"}), Int(4));
}

TEST_F(ListCmds, PopForms) {
  EXPECT_EQ(h.Run({"RPUSH", "l", "a", "b", "c"}), Int(3));
  EXPECT_EQ(h.Run({"LPOP", "l"}), Bulk("a"));
  EXPECT_EQ(h.Run({"RPOP", "l", "1"}), Arr({Bulk("c")}));  // count form is always an array
  EXPECT_EQ(h.Run({"LPOP", "l", "0"}), "*0\r\n");
  EXPECT_EQ(h.Run({"LPOP", "l", "5"}), Arr({Bulk("b")}));
  EXPECT_EQ(h.Run({"EXISTS", "l"}), Int(0));
  EXPECT_EQ(h.Run({"LPOP", "l"}), kNil);
  EXPECT_EQ(h.Run({"LPOP", "l", "0"}), "*-1\r\n");  // missing key with count: null array
  EXPECT_EQ(h.Run({"RPOP", "l", "-1"}), Err(kErrMustBePositive));
  EXPECT_EQ(h.Run({"RPOP", "l", "x"}), Err(kErrMustBePositive));
  EXPECT_EQ(h.Run({"LPOP", "l", "1", "2"}), Err("ERR wrong number of arguments for 'lpop' command"));
}

TEST_F(ListCmds, PopNullsInResp3) {
  h.Resp3();
  EXPECT_EQ(h.Run({"LPOP", "nokey"}), "_\r\n");
  EXPECT_EQ(h.Run({"LPOP", "nokey", "2"}), "_\r\n");
}

TEST_F(ListCmds, BadCountWinsOverWrongType) {  // t_list.c: the count is parsed before the lookup
  EXPECT_EQ(h.Run({"SET", "s", "v"}), kOk);
  EXPECT_EQ(h.Run({"LPOP", "s", "-1"}), Err(kErrMustBePositive));
  EXPECT_EQ(h.Run({"LRANGE", "s", "x", "1"}), Err(kErrNotInteger));
  EXPECT_EQ(h.Run({"LPOP", "s"}), Err(kErrWrongType));
}

TEST_F(ListCmds, Lset) {
  EXPECT_EQ(h.Run({"RPUSH", "l", "a", "b"}), Int(2));
  EXPECT_EQ(h.Run({"LSET", "l", "-1", "B"}), kOk);
  EXPECT_EQ(h.Run({"LRANGE", "l", "0", "-1"}), Arr({Bulk("a"), Bulk("B")}));
  EXPECT_EQ(h.Run({"LSET", "l", "2", "x"}), Err(kErrIndexOutOfRange));
  EXPECT_EQ(h.Run({"LSET", "l", "x", "x"}), Err(kErrNotInteger));
  EXPECT_EQ(h.Run({"LSET", "nokey", "x", "x"}), Err(kErrNoSuchKey));  // the key is checked first
}

// Redis 7.2.5 listTypeReplaceAtIndex takes an `int` index, so LSET truncates the 64-bit argument to
// its low 32 bits (verified against redis:7.2.5: LSET l 9223372036854775807 replaces the last element).
TEST_F(ListCmds, LsetTruncatesIndexToInt32LikeRedis72) {
  EXPECT_EQ(h.Run({"RPUSH", "l", "a", "b", "c"}), Int(3));
  EXPECT_EQ(h.Run({"LSET", "l", "9223372036854775807", "X"}), kOk);   // low 32 bits: -1
  EXPECT_EQ(h.Run({"LSET", "l", "4294967296", "Y"}), kOk);            // 0
  EXPECT_EQ(h.Run({"LSET", "l", "-9223372036854775808", "Z"}), kOk);  // 0
  EXPECT_EQ(h.Run({"LSET", "l", "4294967297", "W"}), kOk);            // 1
  EXPECT_EQ(h.Run({"LRANGE", "l", "0", "-1"}), Arr({Bulk("Z"), Bulk("W"), Bulk("X")}));
  EXPECT_EQ(h.Run({"LSET", "l", "2147483648", "x"}), Err(kErrIndexOutOfRange));  // -2147483648
}

TEST_F(ListCmds, WrongTypeEverywhere) {
  EXPECT_EQ(h.Run({"SET", "s", "v"}), kOk);
  for (const ArgList& cmd : std::vector<ArgList>{{"LPUSH", "s", "a"},
                                                 {"RPUSHX", "s", "a"},
                                                 {"LPOP", "s"},
                                                 {"LLEN", "s"},
                                                 {"LINDEX", "s", "0"},
                                                 {"LSET", "s", "0", "a"},
                                                 {"LRANGE", "s", "0", "1"}}) {
    EXPECT_EQ(h.Run(cmd), Err(kErrWrongType)) << cmd[0];
  }
  EXPECT_EQ(h.Run({"GET", "s"}), Bulk("v"));
}

TEST_F(ListCmds, ExpiredButStoredListIsMissing) {
  EXPECT_EQ(h.Run({"RPUSH", "l", "old"}), Int(1));
  EXPECT_EQ(h.Run({"PEXPIRE", "l", "5"}), Int(1));
  h.clock().Advance(5);
  EXPECT_EQ(h.Run({"LLEN", "l"}), Int(0));
  EXPECT_EQ(h.Run({"LPUSHX", "l", "x"}), Int(0));
  EXPECT_EQ(h.Run({"RPUSH", "l", "new"}), Int(1));
  EXPECT_EQ(h.Run({"LRANGE", "l", "0", "-1"}), Arr({Bulk("new")}));
}

TEST_F(ListCmds, PushKeepsTtlAndPopOfLastDeletesTheKey) {
  EXPECT_EQ(h.Run({"RPUSH", "l", "a"}), Int(1));
  EXPECT_EQ(h.Run({"EXPIRE", "l", "100"}), Int(1));
  EXPECT_EQ(h.Run({"RPUSH", "l", "b"}), Int(2));
  EXPECT_EQ(h.Run({"TTL", "l"}), Int(100));
  EXPECT_EQ(h.Run({"LPOP", "l", "2"}), Arr({Bulk("a"), Bulk("b")}));
  EXPECT_TRUE(h.Cf(ColumnFamily::kMeta).empty());
  EXPECT_TRUE(h.Cf(ColumnFamily::kExpire).empty());
  EXPECT_TRUE(h.Cf(ColumnFamily::kSubkey).empty());
}

TEST_F(ListCmds, BinaryElements) {
  const std::string v("\0\r\n\xff", 4);
  EXPECT_EQ(h.Run({"RPUSH", "l", v, ""}), Int(2));
  EXPECT_EQ(h.Run({"LRANGE", "l", "0", "-1"}), Arr({Bulk(v), Bulk("")}));
}

}  // namespace
}  // namespace tideskv
