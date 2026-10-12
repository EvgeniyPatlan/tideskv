#include <gtest/gtest.h>

#include "commands/reply_util.h"
#include "support/command_harness.h"
#include "types/errors.h"
#include "types/string_type.h"

namespace tideskv {
namespace {

using test::Bulk;
using test::CommandHarness;
using test::Err;
using test::Int;
using test::kNil;
using test::kOk;

struct EditCmds : ::testing::Test {
  CommandHarness h;
  int64_t ExpireOf(std::string_view key) { return h.ExpireOf(0, key); }
};

TEST(RedisRange, MatchesRedisIndexRules) {
  EXPECT_EQ(RedisRange("Hello World", 0, 3), "Hell");
  EXPECT_EQ(RedisRange("Hello World", -3, -1), "rld");
  EXPECT_EQ(RedisRange("Hello World", 5, 3), "");
  EXPECT_EQ(RedisRange("Hello World", 5, 5000), " World");
  EXPECT_EQ(RedisRange("Hello World", -5000, 10000), "Hello World");
  EXPECT_EQ(RedisRange("Hello World", -1, -5), "");  // both negative, start > end
  EXPECT_EQ(RedisRange("", 0, -1), "");
  EXPECT_EQ(RedisRange("bar", 0, 2094967291), "bar");  // redis/redis#1844
}

TEST_F(EditCmds, GetRangeAndSubstr) {
  EXPECT_EQ(h.Run({"GETRANGE", "k", "0", "-1"}), Bulk(""));  // missing: empty bulk, not nil
  EXPECT_EQ(h.Run({"SET", "k", "Hello World"}), kOk);
  EXPECT_EQ(h.Run({"GETRANGE", "k", "0", "4"}), Bulk("Hello"));
  EXPECT_EQ(h.Run({"SUBSTR", "k", "-5", "-1"}), Bulk("World"));
  EXPECT_EQ(h.Run({"GETRANGE", "k", "a", "1"}), Err(kErrNotInteger));
  h.SeedHash(0, "h", 5, false);
  EXPECT_EQ(h.Run({"GETRANGE", "h", "0", "1"}), Err(kErrWrongType));
  EXPECT_EQ(h.Run({"GETRANGE", "h", "x", "1"}), Err(kErrNotInteger));  // indexes parsed first
}

TEST_F(EditCmds, AppendCreatesAndExtendsKeepingTtl) {
  const int64_t now = h.clock().NowMs();
  EXPECT_EQ(h.Run({"APPEND", "k", "Hello"}), Int(5));
  EXPECT_EQ(ExpireOf("k"), -1);
  EXPECT_EQ(h.Run({"SET", "t", "ab", "PX", "1000"}), kOk);
  EXPECT_EQ(h.Run({"APPEND", "t", "cd"}), Int(4));
  EXPECT_EQ(h.Run({"GET", "t"}), Bulk("abcd"));
  EXPECT_EQ(ExpireOf("t"), now + 1000);
  const std::string bin("\0\r\n", 3);
  EXPECT_EQ(h.Run({"APPEND", "t", bin}), Int(7));
  h.SeedHash(0, "h", 5, false);
  EXPECT_EQ(h.Run({"APPEND", "h", "x"}), Err(kErrWrongType));
}

TEST_F(EditCmds, AppendToExpiredKeyStartsFresh) {  // Review Focus #1
  EXPECT_EQ(h.Run({"SET", "k", "old", "PX", "10"}), kOk);
  h.clock().Advance(10);
  EXPECT_EQ(h.Run({"APPEND", "k", "new"}), Int(3));
  EXPECT_EQ(ExpireOf("k"), -1);
}

TEST_F(EditCmds, SetRange) {
  EXPECT_EQ(h.Run({"SETRANGE", "k", "0", ""}), Int(0));
  EXPECT_EQ(h.Run({"GET", "k"}), kNil);  // empty value never creates the key
  EXPECT_EQ(h.Run({"SETRANGE", "k", "1", "foo"}), Int(4));
  EXPECT_EQ(h.Run({"GET", "k"}), Bulk(std::string("\0foo", 4)));
  EXPECT_EQ(h.Run({"SETRANGE", "k", "0", "b"}), Int(4));
  EXPECT_EQ(h.Run({"GET", "k"}), Bulk("bfoo"));
  EXPECT_EQ(h.Run({"SETRANGE", "k", "10", ""}), Int(4));  // empty value: length only
  EXPECT_EQ(h.Run({"SETRANGE", "k", "-1", "x"}), Err(kErrOffsetOutOfRange));
  EXPECT_EQ(h.Run({"SETRANGE", "k", "x", "x"}), Err(kErrNotInteger));
  EXPECT_EQ(h.Run({"SETRANGE", "k", "536870911", "xx"}), Err(kErrStringTooLong));  // 512 MiB + 1
  EXPECT_EQ(h.Run({"GET", "k"}), Bulk("bfoo"));  // unchanged by the rejected write
}

TEST_F(EditCmds, SetRangeWrongTypeAfterOffsetChecks) {
  h.SeedHash(0, "h", 5, false);
  EXPECT_EQ(h.Run({"SETRANGE", "h", "-1", "x"}), Err(kErrOffsetOutOfRange));
  EXPECT_EQ(h.Run({"SETRANGE", "h", "0", "x"}), Err(kErrWrongType));
}

TEST_F(EditCmds, SetRangeKeepsTtl) {
  const int64_t now = h.clock().NowMs();
  EXPECT_EQ(h.Run({"SET", "k", "hello", "PX", "500"}), kOk);
  EXPECT_EQ(h.Run({"SETRANGE", "k", "0", "J"}), Int(5));
  EXPECT_EQ(ExpireOf("k"), now + 500);
}

TEST_F(EditCmds, GetEx) {
  const int64_t now = h.clock().NowMs();
  EXPECT_EQ(h.Run({"GETEX", "k"}), kNil);
  EXPECT_EQ(h.Run({"SET", "k", "bar", "EX", "10"}), kOk);
  EXPECT_EQ(h.Run({"GETEX", "k"}), Bulk("bar"));
  EXPECT_EQ(ExpireOf("k"), now + 10'000);  // no option: TTL untouched
  EXPECT_EQ(h.Run({"GETEX", "k", "PX", "300"}), Bulk("bar"));
  EXPECT_EQ(ExpireOf("k"), now + 300);
  EXPECT_EQ(h.Run({"GETEX", "k", "ex", "1", "ex", "10"}), Bulk("bar"));  // duplicate: last wins
  EXPECT_EQ(ExpireOf("k"), now + 10'000);
  EXPECT_EQ(h.Run({"GETEX", "k", "PERSIST"}), Bulk("bar"));
  EXPECT_EQ(ExpireOf("k"), -1);
  EXPECT_TRUE(h.Cf(ColumnFamily::kExpire).empty());
  EXPECT_EQ(h.Run({"GETEX", "k", "PXAT", "1"}), Bulk("bar"));  // past deadline: deleted, value returned
  EXPECT_EQ(h.Run({"GET", "k"}), kNil);
}

TEST_F(EditCmds, GetExErrors) {
  const std::string bad_expire = "ERR invalid expire time in 'getex' command";
  EXPECT_EQ(h.Run({"GETEX", "k", "EX", "10", "PERSIST"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"GETEX", "k", "non-existent-option"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"GETEX"}), Err("ERR wrong number of arguments for 'getex' command"));
  // Redis 7.2 order: syntax, then lookup/type, then the expire value.
  EXPECT_EQ(h.Run({"GETEX", "k", "EX", "0"}), kNil);
  h.SeedHash(0, "h", 5, false);
  EXPECT_EQ(h.Run({"GETEX", "h"}), Err(kErrWrongType));
  EXPECT_EQ(h.Run({"GETEX", "h", "EX", "0"}), Err(kErrWrongType));
  EXPECT_EQ(h.Run({"SET", "k", "v"}), kOk);
  EXPECT_EQ(h.Run({"GETEX", "k", "EX", "0"}), Err(bad_expire));
  EXPECT_EQ(h.Run({"GET", "k"}), Bulk("v"));  // rolled back
}

}  // namespace
}  // namespace tideskv
