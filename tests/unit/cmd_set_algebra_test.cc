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

struct SetAlgebraCmds : ::testing::Test {
  void SetUp() override {
    EXPECT_EQ(h.Run({"SADD", "a", "1", "2", "3"}), Int(3));
    EXPECT_EQ(h.Run({"SADD", "b", "2", "3", "4"}), Int(3));
  }
  CommandHarness h;
};

TEST_F(SetAlgebraCmds, Reads) {
  EXPECT_EQ(h.Run({"SINTER", "a", "b"}), Arr({Bulk("2"), Bulk("3")}));
  EXPECT_EQ(h.Run({"SUNION", "a", "b"}), Arr({Bulk("1"), Bulk("2"), Bulk("3"), Bulk("4")}));
  EXPECT_EQ(h.Run({"SDIFF", "a", "b"}), Arr({Bulk("1")}));
  EXPECT_EQ(h.Run({"SINTER", "a", "nope"}), "*0\r\n");
  h.Resp3();
  EXPECT_EQ(h.Run({"SDIFF", "a", "b"}), "~1\r\n" + Bulk("1"));
  EXPECT_EQ(h.Run({"SINTER", "a", "nope"}), "~0\r\n");
}

TEST_F(SetAlgebraCmds, WrongTypeAnywhereWinsEvenAfterAMissingKey) {  // Review Focus #3
  EXPECT_EQ(h.Run({"SET", "str", "v"}), kOk);
  for (const char* cmd : {"SINTER", "SUNION", "SDIFF"}) {
    EXPECT_EQ(h.Run({cmd, "nope", "str"}), Err(kErrWrongType)) << cmd;
    EXPECT_EQ(h.Run({cmd, "a", "str"}), Err(kErrWrongType)) << cmd;
  }
  EXPECT_EQ(h.Run({"SINTERCARD", "2", "nope", "str"}), Err(kErrWrongType));
  EXPECT_EQ(h.Run({"SET", "dst", "keep"}), kOk);
  for (const char* cmd : {"SINTERSTORE", "SUNIONSTORE", "SDIFFSTORE"}) {
    EXPECT_EQ(h.Run({cmd, "dst", "a", "str"}), Err(kErrWrongType)) << cmd;
    EXPECT_EQ(h.Run({"GET", "dst"}), Bulk("keep")) << cmd;  // dst untouched
  }
}

TEST_F(SetAlgebraCmds, StoreReplacesAnyDstAndDropsItsTtl) {
  EXPECT_EQ(h.Run({"SET", "dst", "v", "EX", "100"}), kOk);
  EXPECT_EQ(h.Run({"SINTERSTORE", "dst", "a", "b"}), Int(2));
  EXPECT_EQ(h.Run({"TYPE", "dst"}), "+set\r\n");
  EXPECT_EQ(h.Run({"TTL", "dst"}), Int(-1));
  EXPECT_EQ(h.Run({"SMEMBERS", "dst"}), Arr({Bulk("2"), Bulk("3")}));
  EXPECT_EQ(h.Run({"SUNIONSTORE", "dst", "a", "b"}), Int(4));
  EXPECT_EQ(h.Run({"SDIFFSTORE", "dst", "a", "b"}), Int(1));
  EXPECT_EQ(h.Run({"SMEMBERS", "dst"}), Arr({Bulk("1")}));
}

TEST_F(SetAlgebraCmds, StoreOntoAHashDropsItsFieldsAndMakesASet) {
  for (const char* cmd : {"SINTERSTORE", "SUNIONSTORE", "SDIFFSTORE"}) {
    EXPECT_EQ(h.Run({"HSET", "dst", "f1", "x", "f2", "y", "f3", "z"}), Int(3)) << cmd;
    EXPECT_EQ(h.Cf(ColumnFamily::kSubkey).size(), 9u) << cmd;  // a, b and the hash's fields
    const std::string reply = h.Run({cmd, "dst", "a", "b"});
    EXPECT_EQ(reply.substr(0, 1), ":") << cmd;
    EXPECT_EQ(h.Run({"TYPE", "dst"}), "+set\r\n") << cmd;
    EXPECT_EQ(h.Run({"HGET", "dst", "f1"}), Err(kErrWrongType)) << cmd;
    const int64_t members = std::stoll(reply.substr(1));
    EXPECT_EQ(h.Cf(ColumnFamily::kSubkey).size(), 6u + static_cast<size_t>(members)) << cmd;  // fields gone
    EXPECT_EQ(h.Run({"DEL", "dst"}), Int(1)) << cmd;
  }
}

TEST_F(SetAlgebraCmds, EmptyResultDeletesDst) {  // Review Focus #2
  EXPECT_EQ(h.Run({"SADD", "dst", "x"}), Int(1));
  EXPECT_EQ(h.Run({"SINTERSTORE", "dst", "a", "nope"}), Int(0));
  EXPECT_EQ(h.Run({"EXISTS", "dst"}), Int(0));
  EXPECT_EQ(h.Run({"SDIFFSTORE", "dst", "a", "a"}), Int(0));
  EXPECT_EQ(h.Run({"EXISTS", "dst"}), Int(0));
  EXPECT_EQ(h.Cf(ColumnFamily::kSubkey).size(), 6u);  // only a's and b's members remain
}

TEST_F(SetAlgebraCmds, DstMayBeASource) {
  EXPECT_EQ(h.Run({"SINTERSTORE", "a", "a", "b"}), Int(2));
  EXPECT_EQ(h.Run({"SMEMBERS", "a"}), Arr({Bulk("2"), Bulk("3")}));
}

TEST_F(SetAlgebraCmds, ExpiredSourceIsEmpty) {  // Review Focus #1
  EXPECT_EQ(h.Run({"PEXPIRE", "b", "5"}), Int(1));
  h.clock().Advance(5);
  EXPECT_EQ(h.Run({"SINTER", "a", "b"}), "*0\r\n");
  EXPECT_EQ(h.Run({"SUNION", "a", "b"}), Arr({Bulk("1"), Bulk("2"), Bulk("3")}));
}

TEST_F(SetAlgebraCmds, SinterCard) {
  EXPECT_EQ(h.Run({"SINTERCARD", "2", "a", "b"}), Int(2));
  EXPECT_EQ(h.Run({"SINTERCARD", "2", "a", "b", "LIMIT", "1"}), Int(1));
  EXPECT_EQ(h.Run({"SINTERCARD", "2", "a", "b", "limit", "0"}), Int(2));
  EXPECT_EQ(h.Run({"SINTERCARD", "1", "nope"}), Int(0));
  EXPECT_EQ(h.Run({"SINTERCARD", "0", "a"}), Err(kErrNumkeysNotPositive));
  EXPECT_EQ(h.Run({"SINTERCARD", "x", "a"}), Err(kErrNumkeysNotPositive));
  EXPECT_EQ(h.Run({"SINTERCARD", "3", "a", "b"}), Err(kErrNumkeysTooMany));
  EXPECT_EQ(h.Run({"SINTERCARD", "2", "a", "b", "LIMIT", "-1"}), Err(kErrLimitNegative));
  EXPECT_EQ(h.Run({"SINTERCARD", "2", "a", "b", "LIMIT", "x"}), Err(kErrLimitNegative));
  EXPECT_EQ(h.Run({"SINTERCARD", "2", "a", "b", "LIMIT"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"SINTERCARD", "1", "a", "b"}), Err(kErrSyntax));  // "b" is not an option
}

}  // namespace
}  // namespace tideskv
