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

struct HashCmds : ::testing::Test {
  CommandHarness h;
};

TEST_F(HashCmds, SetGetAndArity) {
  EXPECT_EQ(h.Run({"HSET", "h", "f", "v", "g", "w"}), Int(2));
  EXPECT_EQ(h.Run({"HSET", "h", "f", "v2"}), Int(0));
  EXPECT_EQ(h.Run({"HGET", "h", "f"}), Bulk("v2"));
  EXPECT_EQ(h.Run({"HGET", "h", "nope"}), kNil);
  EXPECT_EQ(h.Run({"HGET", "nokey", "f"}), kNil);
  EXPECT_EQ(h.Run({"HSET", "h", "f", "v", "g"}), Err("ERR wrong number of arguments for 'hset' command"));
  EXPECT_EQ(h.Run({"HSET", "h", "f"}), Err("ERR wrong number of arguments for 'hset' command"));
  EXPECT_EQ(h.Run({"HMSET", "h", "a", "1", "b"}), Err("ERR wrong number of arguments for 'hmset' command"));
  EXPECT_EQ(h.Run({"HMSET", "h", "a", "1", "b", "2"}), kOk);
  EXPECT_EQ(h.Run({"HLEN", "h"}), Int(4));
}

TEST_F(HashCmds, MGetSetNxStrlenExists) {
  EXPECT_EQ(h.Run({"HSET", "h", "f", "hello"}), Int(1));
  EXPECT_EQ(h.Run({"HMGET", "h", "f", "nope", "f"}), Arr({Bulk("hello"), kNil, Bulk("hello")}));
  EXPECT_EQ(h.Run({"HMGET", "nokey", "a"}), Arr({kNil}));
  EXPECT_EQ(h.Run({"HSETNX", "h", "f", "x"}), Int(0));
  EXPECT_EQ(h.Run({"HSETNX", "h", "g", "x"}), Int(1));
  EXPECT_EQ(h.Run({"HSTRLEN", "h", "f"}), Int(5));
  EXPECT_EQ(h.Run({"HSTRLEN", "h", "nope"}), Int(0));
  EXPECT_EQ(h.Run({"HEXISTS", "h", "g"}), Int(1));
  EXPECT_EQ(h.Run({"HEXISTS", "h", "nope"}), Int(0));
  EXPECT_EQ(h.Run({"HEXISTS", "nokey", "g"}), Int(0));
}

TEST_F(HashCmds, BinaryAndEmptyFieldsRoundTrip) {
  const std::string field("\0\r\n\xff", 4);
  EXPECT_EQ(h.Run({"HSET", "h", "", "empty", field, "bin"}), Int(2));
  EXPECT_EQ(h.Run({"HGET", "h", ""}), Bulk("empty"));
  EXPECT_EQ(h.Run({"HGET", "h", field}), Bulk("bin"));
  EXPECT_EQ(h.Run({"HSET", "", "f", ""}), Int(1));  // empty key and empty value
  EXPECT_EQ(h.Run({"HGET", "", "f"}), Bulk(""));
}

TEST_F(HashCmds, LastFieldDeletesTheKey) {  // Review Focus #2
  EXPECT_EQ(h.Run({"HSET", "h", "f", "v", "g", "w"}), Int(2));
  EXPECT_EQ(h.Run({"PEXPIRE", "h", "100000"}), Int(1));
  EXPECT_EQ(h.Run({"HDEL", "h", "f", "f", "nope"}), Int(1));
  EXPECT_EQ(h.Run({"HDEL", "h", "g"}), Int(1));
  EXPECT_EQ(h.Run({"EXISTS", "h"}), Int(0));
  EXPECT_EQ(h.Run({"TYPE", "h"}), "+none\r\n");
  EXPECT_TRUE(h.Cf(ColumnFamily::kMeta).empty());
  EXPECT_TRUE(h.Cf(ColumnFamily::kExpire).empty());
  EXPECT_TRUE(h.Cf(ColumnFamily::kSubkey).empty());
  EXPECT_EQ(h.Run({"HDEL", "h", "g"}), Int(0));
}

TEST_F(HashCmds, HsetKeepsTtl) {  // Review Focus #2
  EXPECT_EQ(h.Run({"HSET", "h", "f", "v"}), Int(1));
  EXPECT_EQ(h.Run({"PEXPIRE", "h", "5000"}), Int(1));
  const int64_t deadline = h.ExpireOf(0, "h");
  EXPECT_EQ(h.Run({"HSET", "h", "g", "w"}), Int(1));
  EXPECT_EQ(h.Run({"HDEL", "h", "f"}), Int(1));
  EXPECT_EQ(h.ExpireOf(0, "h"), deadline);
  EXPECT_EQ(h.Run({"PTTL", "h"}), Int(5000));
}

TEST_F(HashCmds, ExpiredButStoredHashIsMissingAndRecreatedFresh) {  // Review Focus #1
  EXPECT_EQ(h.Run({"HSET", "h", "old", "v"}), Int(1));
  EXPECT_EQ(h.Run({"PEXPIRE", "h", "10"}), Int(1));
  h.clock().Advance(10);
  EXPECT_EQ(h.Run({"HLEN", "h"}), Int(0));
  EXPECT_EQ(h.Run({"HGET", "h", "old"}), kNil);
  EXPECT_EQ(h.Run({"HEXISTS", "h", "old"}), Int(0));
  EXPECT_EQ(h.Run({"HSET", "h", "new", "w"}), Int(1));
  EXPECT_EQ(h.Run({"HGET", "h", "old"}), kNil);
  EXPECT_EQ(h.Run({"HLEN", "h"}), Int(1));
  EXPECT_EQ(h.Run({"PTTL", "h"}), Int(-1));  // the recreated hash has no deadline
  EXPECT_EQ(h.Cf(ColumnFamily::kSubkey).size(), 1u);
}

TEST_F(HashCmds, WrongTypeEverywhere) {  // Review Focus #3
  EXPECT_EQ(h.Run({"SET", "s", "v"}), kOk);
  const std::string wt = Err(kErrWrongType);
  for (const ArgList& cmd : std::vector<ArgList>{{"HSET", "s", "f", "v"},
                                                 {"HMSET", "s", "f", "v"},
                                                 {"HSETNX", "s", "f", "v"},
                                                 {"HGET", "s", "f"},
                                                 {"HMGET", "s", "f"},
                                                 {"HDEL", "s", "f"},
                                                 {"HLEN", "s"},
                                                 {"HSTRLEN", "s", "f"},
                                                 {"HEXISTS", "s", "f"}}) {
    EXPECT_EQ(h.Run(cmd), wt) << cmd[0];
  }
  EXPECT_EQ(h.Run({"GET", "s"}), Bulk("v"));
  EXPECT_TRUE(h.Cf(ColumnFamily::kSubkey).empty());
}

TEST_F(HashCmds, SetOverAHashDropsItsFields) {  // Review Focus #3
  EXPECT_EQ(h.Run({"HSET", "h", "f", "v"}), Int(1));
  EXPECT_EQ(h.Run({"SET", "h", "s"}), kOk);
  EXPECT_TRUE(h.Cf(ColumnFamily::kSubkey).empty());
  EXPECT_EQ(h.Run({"HGET", "h", "f"}), Err(kErrWrongType));
}

TEST_F(HashCmds, HashesArePerDb) {
  EXPECT_EQ(h.Run({"HSET", "h", "f", "db0"}), Int(1));
  h.session.db = 3;
  EXPECT_EQ(h.Run({"HGET", "h", "f"}), kNil);
  EXPECT_EQ(h.Run({"HSET", "h", "f", "db3"}), Int(1));
  h.session.db = 0;
  EXPECT_EQ(h.Run({"HGET", "h", "f"}), Bulk("db0"));
}

}  // namespace
}  // namespace tideskv
