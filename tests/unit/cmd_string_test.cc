#include <gtest/gtest.h>

#include "commands/reply_util.h"
#include "support/command_harness.h"

namespace tideskv {
namespace {

using test::Arr;
using test::Bulk;
using test::CommandHarness;
using test::Err;
using test::Int;
using test::kNil;
using test::kOk;

struct StringCmds : ::testing::Test {
  CommandHarness h;
};

TEST_F(StringCmds, GetSetBasics) {
  EXPECT_EQ(h.Run({"GET", "k"}), kNil);
  EXPECT_EQ(h.Run({"SET", "k", "v"}), kOk);
  EXPECT_EQ(h.Run({"GET", "k"}), Bulk("v"));
  EXPECT_EQ(h.Run({"SET", "k", ""}), kOk);
  EXPECT_EQ(h.Run({"GET", "k"}), Bulk(""));
}

TEST_F(StringCmds, BinaryKeysAndValues) {  // Review Focus #4
  const std::string key("k\0\r\n\xff", 5), value("\0\x01\r\n", 4);
  EXPECT_EQ(h.Run({"SET", key, value}), kOk);
  EXPECT_EQ(h.Run({"GET", key}), Bulk(value));
  EXPECT_EQ(h.Run({"SET", "", "empty-key"}), kOk);
  EXPECT_EQ(h.Run({"GET", ""}), Bulk("empty-key"));
}

TEST_F(StringCmds, DatabasesAreSeparate) {
  EXPECT_EQ(h.Run({"SET", "k", "zero"}), kOk);
  h.session.db = 3;
  EXPECT_EQ(h.Run({"GET", "k"}), kNil);
  EXPECT_EQ(h.Run({"SET", "k", "three"}), kOk);
  h.session.db = 0;
  EXPECT_EQ(h.Run({"GET", "k"}), Bulk("zero"));
}

TEST_F(StringCmds, WrongType) {
  h.SeedHash(0, "h", 77, /*with_field=*/true);
  ASSERT_EQ(h.Cf(ColumnFamily::kSubkey).size(), 1u);
  EXPECT_EQ(h.Run({"GET", "h"}), Err(kErrWrongType));
  EXPECT_EQ(h.Run({"STRLEN", "h"}), Err(kErrWrongType));
  EXPECT_EQ(h.Run({"GETSET", "h", "x"}), Err(kErrWrongType));
  EXPECT_EQ(h.Run({"GETDEL", "h"}), Err(kErrWrongType));
  EXPECT_EQ(h.Run({"SET", "h", "x", "GET"}), Err(kErrWrongType));
  EXPECT_EQ(h.Run({"MGET", "h", "nope"}), Arr({kNil, kNil}));  // MGET never errors
  EXPECT_EQ(h.Run({"SET", "h", "x"}), kOk);                    // plain SET overwrites any type
  EXPECT_EQ(h.Run({"GET", "h"}), Bulk("x"));
  EXPECT_TRUE(h.Cf(ColumnFamily::kSubkey).empty());  // the overwrite dropped the hash field
}

TEST_F(StringCmds, SetNxXx) {
  EXPECT_EQ(h.Run({"SET", "k", "1", "xx"}), kNil);
  EXPECT_EQ(h.Run({"GET", "k"}), kNil);
  EXPECT_EQ(h.Run({"SET", "k", "1", "nx"}), kOk);
  EXPECT_EQ(h.Run({"SET", "k", "2", "NX"}), kNil);
  EXPECT_EQ(h.Run({"GET", "k"}), Bulk("1"));
  EXPECT_EQ(h.Run({"SET", "k", "2", "XX"}), kOk);
  EXPECT_EQ(h.Run({"GET", "k"}), Bulk("2"));
}

TEST_F(StringCmds, SetGetOption) {
  EXPECT_EQ(h.Run({"SET", "k", "bar", "GET"}), kNil);
  EXPECT_EQ(h.Run({"SET", "k", "xx", "GET"}), Bulk("bar"));
  EXPECT_EQ(h.Run({"GET", "k"}), Bulk("xx"));
  EXPECT_EQ(h.Run({"SET", "k", "yy", "NX", "GET"}), Bulk("xx"));  // condition fails: old value, no write
  EXPECT_EQ(h.Run({"GET", "k"}), Bulk("xx"));
  EXPECT_EQ(h.Run({"SET", "n", "v", "XX", "GET"}), kNil);
  EXPECT_EQ(h.Run({"GET", "n"}), kNil);
}

TEST_F(StringCmds, SetExpireOptions) {
  const int64_t now = h.clock().NowMs();
  EXPECT_EQ(h.Run({"SET", "a", "v", "EX", "10"}), kOk);
  EXPECT_EQ(h.ExpireOf(0, "a"), now + 10'000);
  EXPECT_EQ(h.Run({"SET", "b", "v", "PX", "500"}), kOk);
  EXPECT_EQ(h.ExpireOf(0, "b"), now + 500);
  EXPECT_EQ(h.Run({"SET", "c", "v", "EXAT", std::to_string(now / 1000 + 100)}), kOk);
  EXPECT_EQ(h.ExpireOf(0, "c"), (now / 1000 + 100) * 1000);
  EXPECT_EQ(h.Run({"SET", "d", "v", "PXAT", std::to_string(now + 7)}), kOk);
  EXPECT_EQ(h.ExpireOf(0, "d"), now + 7);
  EXPECT_EQ(h.Run({"SET", "a", "v2"}), kOk);  // no KEEPTTL: TTL cleared
  EXPECT_EQ(h.ExpireOf(0, "a"), -1);
}

TEST_F(StringCmds, SetKeepTtl) {
  const int64_t now = h.clock().NowMs();
  EXPECT_EQ(h.Run({"SET", "k", "v", "EX", "10"}), kOk);
  EXPECT_EQ(h.Run({"SET", "k", "v2", "KEEPTTL"}), kOk);
  EXPECT_EQ(h.ExpireOf(0, "k"), now + 10'000);
  EXPECT_EQ(h.Run({"SET", "k", "v3", "GET", "KEEPTTL"}), Bulk("v2"));
  EXPECT_EQ(h.ExpireOf(0, "k"), now + 10'000);
  EXPECT_EQ(h.Run({"SET", "fresh", "v", "KEEPTTL"}), kOk);
  EXPECT_EQ(h.ExpireOf(0, "fresh"), -1);
}

TEST_F(StringCmds, SetExatInThePastDeletes) {
  EXPECT_EQ(h.Run({"SET", "k", "bar"}), kOk);
  EXPECT_EQ(h.Run({"SET", "k", "bar", "EXAT", "1"}), kOk);
  EXPECT_EQ(h.Run({"GET", "k"}), kNil);
  EXPECT_TRUE(h.Cf(ColumnFamily::kMeta).empty());
  EXPECT_TRUE(h.Cf(ColumnFamily::kExpire).empty());
  EXPECT_EQ(h.Run({"SET", "k", "bar"}), kOk);
  EXPECT_EQ(h.Run({"SET", "k", "new", "PXAT", "1", "GET"}), Bulk("bar"));
  EXPECT_EQ(h.Run({"GET", "k"}), kNil);
}

TEST_F(StringCmds, SetErrors) {
  EXPECT_EQ(h.Run({"SET", "k", "v", "EX", "0"}), Err(InvalidExpireError("set")));
  EXPECT_EQ(h.Run({"SET", "k", "v", "EX", "1234xyz"}), Err(kErrNotInteger));
  EXPECT_EQ(h.Run({"SET", "k", "v", "PXAT", "0"}), Err(InvalidExpireError("set")));
  EXPECT_EQ(h.Run({"SET", "k", "v", "EX", "9223372036854775807"}), Err(InvalidExpireError("set")));
  EXPECT_EQ(h.Run({"SET", "k", "v", "NX", "XX"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"SET", "k", "v", "bogus"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"SET", "k"}), Err(ArityError("set")));
  EXPECT_EQ(h.Run({"GET", "k"}), kNil);  // nothing was written by any failed SET
}

TEST_F(StringCmds, ExpiredButStoredBehavesAsMissing) {  // Review Focus #1
  EXPECT_EQ(h.Run({"SET", "k", "old", "PX", "100"}), kOk);
  h.clock().Advance(100);
  EXPECT_EQ(h.Run({"GET", "k"}), kNil);
  EXPECT_EQ(h.Run({"STRLEN", "k"}), Int(0));
  EXPECT_EQ(h.Run({"MGET", "k"}), Arr({kNil}));
  EXPECT_EQ(h.Run({"SETNX", "k", "new"}), Int(1));
  EXPECT_EQ(h.Run({"GET", "k"}), Bulk("new"));
  EXPECT_TRUE(h.Cf(ColumnFamily::kExpire).empty());  // stale index entry removed by the writer
  EXPECT_EQ(h.Run({"SET", "k2", "old", "PX", "100", "GET"}), kNil);
  h.clock().Advance(100);
  EXPECT_EQ(h.Run({"SET", "k2", "new", "GET"}), kNil);  // GET reports the expired value as absent
}

TEST_F(StringCmds, SetNxSetExPsetEx) {
  const int64_t now = h.clock().NowMs();
  EXPECT_EQ(h.Run({"SETNX", "k", "1"}), Int(1));
  EXPECT_EQ(h.Run({"SETNX", "k", "2"}), Int(0));
  EXPECT_EQ(h.Run({"GET", "k"}), Bulk("1"));
  EXPECT_EQ(h.Run({"SETEX", "e", "10", "v"}), kOk);
  EXPECT_EQ(h.ExpireOf(0, "e"), now + 10'000);
  EXPECT_EQ(h.Run({"PSETEX", "p", "250", "v"}), kOk);
  EXPECT_EQ(h.ExpireOf(0, "p"), now + 250);
  EXPECT_EQ(h.Run({"SETEX", "e", "0", "v"}), Err(InvalidExpireError("setex")));
  EXPECT_EQ(h.Run({"PSETEX", "p", "-1", "v"}), Err(InvalidExpireError("psetex")));
  EXPECT_EQ(h.Run({"SETEX", "e", "ten", "v"}), Err(kErrNotInteger));
}

TEST_F(StringCmds, GetSetAndGetDel) {
  EXPECT_EQ(h.Run({"GETSET", "k", "a"}), kNil);
  EXPECT_EQ(h.Run({"SET", "k", "a", "EX", "100"}), kOk);
  EXPECT_EQ(h.Run({"GETSET", "k", "b"}), Bulk("a"));
  EXPECT_EQ(h.ExpireOf(0, "k"), -1);  // GETSET clears the TTL
  EXPECT_EQ(h.Run({"GETDEL", "k"}), Bulk("b"));
  EXPECT_EQ(h.Run({"GETDEL", "k"}), kNil);
  EXPECT_EQ(h.Run({"GET", "k"}), kNil);
}

TEST_F(StringCmds, MsetMgetMsetnx) {
  EXPECT_EQ(h.Run({"MSET", "a", "1", "b", "2", "a", "3"}), kOk);  // duplicate: last wins
  EXPECT_EQ(h.Run({"MGET", "a", "b", "c"}), Arr({Bulk("3"), Bulk("2"), kNil}));
  EXPECT_EQ(h.Run({"MSETNX", "c", "x", "a", "y"}), Int(0));  // a exists: nothing written
  EXPECT_EQ(h.Run({"GET", "c"}), kNil);
  EXPECT_EQ(h.Run({"MSETNX", "c", "x", "d", "y", "c", "z"}), Int(1));
  EXPECT_EQ(h.Run({"MGET", "c", "d"}), Arr({Bulk("z"), Bulk("y")}));
  EXPECT_EQ(h.Run({"MSET", "a", "1", "b"}), Err(ArityError("mset")));
  EXPECT_EQ(h.Run({"MSETNX", "a"}), Err(ArityError("msetnx")));
}

TEST_F(StringCmds, MsetClearsTtl) {
  EXPECT_EQ(h.Run({"SET", "a", "1", "EX", "100"}), kOk);
  EXPECT_EQ(h.Run({"MSET", "a", "2"}), kOk);
  EXPECT_EQ(h.ExpireOf(0, "a"), -1);
}

TEST_F(StringCmds, Strlen) {
  EXPECT_EQ(h.Run({"STRLEN", "k"}), Int(0));
  EXPECT_EQ(h.Run({"SET", "k", "hello"}), kOk);
  EXPECT_EQ(h.Run({"STRLEN", "k"}), Int(5));
}

TEST_F(StringCmds, CommandTableEntries) {
  const CommandSpec* mset = h.services.registry().Find("mset");
  ASSERT_NE(mset, nullptr);
  EXPECT_EQ(mset->arity, -3);
  EXPECT_EQ(mset->first_key, 1);
  EXPECT_EQ(mset->last_key, -1);
  EXPECT_EQ(mset->key_step, 2);
  EXPECT_TRUE(mset->flags & kCmdWrite);
  EXPECT_TRUE(h.services.registry().Find("get")->flags & kCmdReadOnly);
}

}  // namespace
}  // namespace tideskv
