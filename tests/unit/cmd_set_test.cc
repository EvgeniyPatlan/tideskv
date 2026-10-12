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
using test::kOk;

struct SetCmds : ::testing::Test {
  CommandHarness h;
};

TEST_F(SetCmds, AddRemCardIsMember) {
  EXPECT_EQ(h.Run({"SADD", "s", "b", "a", "b"}), Int(2));
  EXPECT_EQ(h.Run({"SCARD", "s"}), Int(2));
  EXPECT_EQ(h.Run({"SISMEMBER", "s", "a"}), Int(1));
  EXPECT_EQ(h.Run({"SISMEMBER", "s", "z"}), Int(0));
  EXPECT_EQ(h.Run({"SISMEMBER", "nokey", "a"}), Int(0));
  EXPECT_EQ(h.Run({"SMISMEMBER", "s", "a", "z", "b"}), Arr({Int(1), Int(0), Int(1)}));
  EXPECT_EQ(h.Run({"SMISMEMBER", "nokey", "a"}), Arr({Int(0)}));
  EXPECT_EQ(h.Run({"SMEMBERS", "s"}), Arr({Bulk("a"), Bulk("b")}));
  EXPECT_EQ(h.Run({"SMEMBERS", "nokey"}), "*0\r\n");
  EXPECT_EQ(h.Run({"SREM", "s", "a", "z"}), Int(1));
  EXPECT_EQ(h.Run({"SREM", "nokey", "a"}), Int(0));
  EXPECT_EQ(h.Run({"SCARD", "nokey"}), Int(0));
}

TEST_F(SetCmds, SmembersIsASetInResp3) {
  EXPECT_EQ(h.Run({"SADD", "s", "a"}), Int(1));
  h.Resp3();
  EXPECT_EQ(h.Run({"SMEMBERS", "s"}), "~1\r\n" + Bulk("a"));
  EXPECT_EQ(h.Run({"SMEMBERS", "nokey"}), "~0\r\n");
}

TEST_F(SetCmds, BinaryAndEmptyMembers) {
  const std::string m("\0\r\n\xff", 4);
  EXPECT_EQ(h.Run({"SADD", "s", "", m}), Int(2));
  EXPECT_EQ(h.Run({"SISMEMBER", "s", ""}), Int(1));
  EXPECT_EQ(h.Run({"SISMEMBER", "s", m}), Int(1));
}

TEST_F(SetCmds, LastMemberDeletesTheKey) {  // Review Focus #2
  EXPECT_EQ(h.Run({"SADD", "s", "a"}), Int(1));
  EXPECT_EQ(h.Run({"EXPIRE", "s", "100"}), Int(1));
  EXPECT_EQ(h.Run({"SREM", "s", "a"}), Int(1));
  EXPECT_EQ(h.Run({"EXISTS", "s"}), Int(0));
  EXPECT_TRUE(h.Cf(ColumnFamily::kMeta).empty());
  EXPECT_TRUE(h.Cf(ColumnFamily::kExpire).empty());
  EXPECT_TRUE(h.Cf(ColumnFamily::kSubkey).empty());
}

TEST_F(SetCmds, SaddKeepsTtl) {  // Review Focus #2
  EXPECT_EQ(h.Run({"SADD", "s", "a"}), Int(1));
  EXPECT_EQ(h.Run({"PEXPIRE", "s", "7000"}), Int(1));
  EXPECT_EQ(h.Run({"SADD", "s", "b"}), Int(1));
  EXPECT_EQ(h.Run({"SREM", "s", "a"}), Int(1));
  EXPECT_EQ(h.Run({"PTTL", "s"}), Int(7000));
}

TEST_F(SetCmds, ExpiredButStoredSetIsMissing) {  // Review Focus #1
  EXPECT_EQ(h.Run({"SADD", "s", "old"}), Int(1));
  EXPECT_EQ(h.Run({"PEXPIRE", "s", "5"}), Int(1));
  h.clock().Advance(5);
  EXPECT_EQ(h.Run({"SCARD", "s"}), Int(0));
  EXPECT_EQ(h.Run({"SMEMBERS", "s"}), "*0\r\n");
  EXPECT_EQ(h.Run({"SISMEMBER", "s", "old"}), Int(0));
  EXPECT_EQ(h.Run({"SADD", "s", "new"}), Int(1));
  EXPECT_EQ(h.Run({"SMEMBERS", "s"}), Arr({Bulk("new")}));
}

TEST_F(SetCmds, WrongTypeEverywhere) {  // Review Focus #3
  EXPECT_EQ(h.Run({"SET", "str", "v"}), kOk);
  EXPECT_EQ(h.Run({"HSET", "hash", "f", "v"}), Int(1));
  for (const char* key : {"str", "hash"}) {
    for (const ArgList& cmd : std::vector<ArgList>{{"SADD", key, "m"},
                                                   {"SREM", key, "m"},
                                                   {"SCARD", key},
                                                   {"SISMEMBER", key, "m"},
                                                   {"SMISMEMBER", key, "m"},
                                                   {"SMEMBERS", key},
                                                   {"SSCAN", key, "0"}}) {
      EXPECT_EQ(h.Run(cmd), Err(kErrWrongType)) << cmd[0] << " " << key;
    }
  }
  EXPECT_EQ(h.Run({"HGET", "hash", "f"}), Bulk("v"));
}

TEST_F(SetCmds, SpopShapesAndErrors) {
  EXPECT_EQ(h.Run({"SPOP", "nokey"}), test::kNil);
  EXPECT_EQ(h.Run({"SPOP", "nokey", "3"}), "*0\r\n");
  EXPECT_EQ(h.Run({"SADD", "s", "a", "b", "c"}), Int(3));
  EXPECT_EQ(h.Run({"SPOP", "s", "0"}), "*0\r\n");
  EXPECT_EQ(h.Run({"SPOP", "s", "-1"}), Err(kErrMustBePositive));
  EXPECT_EQ(h.Run({"SPOP", "s", "x"}), Err(kErrMustBePositive));
  EXPECT_EQ(h.Run({"SPOP", "s", "1", "2"}), Err(kErrSyntax));
  const std::string one = h.Run({"SPOP", "s"});
  EXPECT_TRUE(one == Bulk("a") || one == Bulk("b") || one == Bulk("c")) << one;
  EXPECT_EQ(h.Run({"SCARD", "s"}), Int(2));
  EXPECT_EQ(h.Run({"SPOP", "s", "5"}).substr(0, 4), "*2\r\n");  // whole set
  EXPECT_EQ(h.Run({"EXISTS", "s"}), Int(0));                    // Review Focus #2
  EXPECT_TRUE(h.Cf(ColumnFamily::kMeta).empty());
}

TEST_F(SetCmds, SpopWithCountIsASetInResp3) {
  EXPECT_EQ(h.Run({"SADD", "s", "a"}), Int(1));
  h.Resp3();
  EXPECT_EQ(h.Run({"SPOP", "nokey", "1"}), "~0\r\n");
  EXPECT_EQ(h.Run({"SPOP", "s", "1"}), "~1\r\n" + Bulk("a"));
}

TEST_F(SetCmds, SrandmemberShapesAndErrors) {
  EXPECT_EQ(h.Run({"SRANDMEMBER", "nokey"}), test::kNil);
  EXPECT_EQ(h.Run({"SRANDMEMBER", "nokey", "2"}), "*0\r\n");
  EXPECT_EQ(h.Run({"SADD", "s", "a", "b"}), Int(2));
  EXPECT_EQ(h.Run({"SRANDMEMBER", "s", "5"}), Arr({Bulk("a"), Bulk("b")}));
  EXPECT_EQ(h.Run({"SRANDMEMBER", "s", "0"}), "*0\r\n");
  EXPECT_EQ(h.Run({"SRANDMEMBER", "s", "-5"}).substr(0, 4), "*5\r\n");
  EXPECT_EQ(h.Run({"SRANDMEMBER", "s", "x"}), Err(kErrNotInteger));
  EXPECT_EQ(h.Run({"SRANDMEMBER", "s", "-9223372036854775808"}), Err(kErrCountOutOfRange));
  EXPECT_EQ(h.Run({"SRANDMEMBER", "s", "1", "2"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"SCARD", "s"}), Int(2));  // read-only
  h.Resp3();
  EXPECT_EQ(h.Run({"SRANDMEMBER", "s", "5"}).substr(0, 4), "*2\r\n");  // an array, not a set
}

TEST_F(SetCmds, SmoveFollowsRedisCheckOrder) {
  EXPECT_EQ(h.Run({"SET", "str", "v"}), kOk);
  EXPECT_EQ(h.Run({"SMOVE", "nokey", "str", "m"}), Int(0));  // missing src wins over a bad dst
  EXPECT_EQ(h.Run({"SADD", "s", "a", "b"}), Int(2));
  EXPECT_EQ(h.Run({"SMOVE", "s", "str", "zz"}), Err(kErrWrongType));  // dst checked before membership
  EXPECT_EQ(h.Run({"SMOVE", "str", "s", "a"}), Err(kErrWrongType));
  EXPECT_EQ(h.Run({"SMOVE", "s", "s", "a"}), Int(1));
  EXPECT_EQ(h.Run({"SMOVE", "s", "s", "zz"}), Int(0));
  EXPECT_EQ(h.Run({"SMOVE", "s", "d", "a"}), Int(1));
  EXPECT_EQ(h.Run({"SMOVE", "s", "d", "b"}), Int(1));
  EXPECT_EQ(h.Run({"EXISTS", "s"}), Int(0));  // Review Focus #2
  EXPECT_EQ(h.Run({"SMEMBERS", "d"}), Arr({Bulk("a"), Bulk("b")}));
  EXPECT_EQ(h.Run({"SADD", "e", "a"}), Int(1));
  EXPECT_EQ(h.Run({"SMOVE", "d", "e", "a"}), Int(1));  // already in dst: still 1
  EXPECT_EQ(h.Run({"SCARD", "e"}), Int(1));
}

TEST_F(SetCmds, SmoveFromExpiredSourceIsZero) {  // Review Focus #1
  EXPECT_EQ(h.Run({"SADD", "s", "a"}), Int(1));
  EXPECT_EQ(h.Run({"PEXPIRE", "s", "5"}), Int(1));
  h.clock().Advance(5);
  EXPECT_EQ(h.Run({"SMOVE", "s", "d", "a"}), Int(0));
  EXPECT_EQ(h.Run({"EXISTS", "d"}), Int(0));
}

}  // namespace
}  // namespace tideskv
