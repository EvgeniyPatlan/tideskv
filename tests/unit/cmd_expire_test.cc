#include <gtest/gtest.h>

#include "commands/reply_util.h"
#include "support/command_harness.h"

namespace tideskv {
namespace {

using test::Bulk;
using test::CommandHarness;
using test::Err;
using test::Int;
using test::kNil;
using test::kOk;

struct ExpireCmds : ::testing::Test {
  CommandHarness h;
  int64_t now = 0;
  void SetUp() override { now = h.clock().NowMs(); }
  std::string Run(std::initializer_list<const char*> a) { return h.Run(ArgList(a.begin(), a.end())); }
  std::string Run(const ArgList& a) { return h.Run(a); }
};

TEST_F(ExpireCmds, ExpireAndTtl) {
  EXPECT_EQ(Run({"EXPIRE", "k", "10"}), Int(0));  // missing key
  EXPECT_EQ(Run({"SET", "k", "v"}), kOk);
  EXPECT_EQ(Run({"TTL", "k"}), Int(-1));
  EXPECT_EQ(Run({"EXPIRE", "k", "10"}), Int(1));
  EXPECT_EQ(Run({"PTTL", "k"}), Int(10'000));
  EXPECT_EQ(Run({"TTL", "k"}), Int(10));
  h.clock().Advance(8'501);
  EXPECT_EQ(Run({"TTL", "k"}), Int(1));  // 1499 ms rounds to 1
  h.clock().Advance(-2);
  EXPECT_EQ(Run({"TTL", "k"}), Int(2));  // 1501 ms rounds to 2
  EXPECT_EQ(Run({"TTL", "nope"}), Int(-2));
  EXPECT_EQ(Run({"PTTL", "nope"}), Int(-2));
}

TEST_F(ExpireCmds, PexpireAndAbsoluteForms) {
  EXPECT_EQ(Run({"SET", "k", "v"}), kOk);
  EXPECT_EQ(Run({"PEXPIRE", "k", "1500"}), Int(1));
  EXPECT_EQ(Run({"PEXPIRETIME", "k"}), Int(now + 1500));
  EXPECT_EQ(Run({"EXPIRETIME", "k"}), Int((now + 1500 + 500) / 1000));
  EXPECT_EQ(Run(ArgList{"EXPIREAT", "k", std::to_string(now / 1000 + 100)}), Int(1));
  EXPECT_EQ(Run({"PEXPIRETIME", "k"}), Int((now / 1000 + 100) * 1000));
  EXPECT_EQ(Run(ArgList{"PEXPIREAT", "k", std::to_string(now + 42)}), Int(1));
  EXPECT_EQ(Run({"PTTL", "k"}), Int(42));
  EXPECT_EQ(Run({"EXPIRETIME", "nope"}), Int(-2));
  EXPECT_EQ(Run({"SET", "p", "v"}), kOk);
  EXPECT_EQ(Run({"PEXPIRETIME", "p"}), Int(-1));
  EXPECT_EQ(h.Cf(ColumnFamily::kExpire).size(), 1u);  // one entry for k, moved on every change
}

TEST_F(ExpireCmds, NonPositiveDeletes) {
  EXPECT_EQ(Run({"SET", "k", "v"}), kOk);
  EXPECT_EQ(Run({"EXPIRE", "k", "-1"}), Int(1));
  EXPECT_EQ(Run({"GET", "k"}), kNil);
  EXPECT_EQ(Run({"SET", "k", "v"}), kOk);
  EXPECT_EQ(Run({"EXPIREAT", "k", "0"}), Int(1));
  EXPECT_EQ(Run({"EXISTS", "k"}), Int(0));
  EXPECT_TRUE(h.Cf(ColumnFamily::kMeta).empty());
  EXPECT_TRUE(h.Cf(ColumnFamily::kExpire).empty());
}

TEST_F(ExpireCmds, Conditions) {
  EXPECT_EQ(Run({"SET", "k", "v"}), kOk);
  EXPECT_EQ(Run({"EXPIRE", "k", "100", "XX"}), Int(0));
  EXPECT_EQ(Run({"EXPIRE", "k", "100", "GT"}), Int(0));  // no deadline = infinite
  EXPECT_EQ(Run({"EXPIRE", "k", "100", "LT"}), Int(1));
  EXPECT_EQ(Run({"EXPIRE", "k", "50", "NX"}), Int(0));
  EXPECT_EQ(Run({"EXPIRE", "k", "200", "lt"}), Int(0));
  EXPECT_EQ(Run({"EXPIRE", "k", "200", "gt"}), Int(1));
  EXPECT_EQ(Run({"EXPIRE", "k", "200", "GT"}), Int(0));  // equal is not greater
  EXPECT_EQ(Run({"EXPIRE", "k", "300", "XX", "GT"}), Int(1));
  EXPECT_EQ(Run({"TTL", "k"}), Int(300));
  EXPECT_EQ(Run({"SET", "k2", "v"}), kOk);
  EXPECT_EQ(Run({"EXPIRE", "k2", "100", "XX", "LT"}), Int(0));  // XX forbids it although LT alone would pass
  EXPECT_EQ(Run({"EXPIRE", "k2", "100", "LT"}), Int(1));
}

TEST_F(ExpireCmds, OptionAndValueErrors) {
  EXPECT_EQ(Run({"SET", "k", "v"}), kOk);
  EXPECT_EQ(Run({"EXPIRE", "k", "10", "NX", "XX"}),
            Err("ERR NX and XX, GT or LT options at the same time are not compatible"));
  EXPECT_EQ(Run({"EXPIRE", "k", "10", "GT", "LT"}),
            Err("ERR GT and LT options at the same time are not compatible"));
  EXPECT_EQ(Run({"EXPIRE", "k", "10", "foo"}), Err("ERR Unsupported option foo"));
  EXPECT_EQ(Run({"EXPIRE", "k", "abc", "foo"}), Err("ERR Unsupported option foo"));  // options first
  EXPECT_EQ(Run({"EXPIRE", "k", ""}), Err(kErrNotInteger));
  EXPECT_EQ(Run({"EXPIRE", "k", "9223372036854775807"}), Err("ERR invalid expire time in 'expire' command"));
  EXPECT_EQ(Run({"PEXPIRE", "k", "9223372036854775807"}),
            Err("ERR invalid expire time in 'pexpire' command"));
  EXPECT_EQ(Run({"EXPIREAT", "k", "9223372036854775807"}),
            Err("ERR invalid expire time in 'expireat' command"));
  EXPECT_EQ(Run({"TTL", "k"}), Int(-1));  // untouched
}

TEST_F(ExpireCmds, Persist) {
  EXPECT_EQ(Run({"PERSIST", "nope"}), Int(0));
  EXPECT_EQ(Run({"SET", "k", "v"}), kOk);
  EXPECT_EQ(Run({"PERSIST", "k"}), Int(0));
  EXPECT_EQ(Run({"EXPIRE", "k", "10"}), Int(1));
  EXPECT_EQ(Run({"PERSIST", "k"}), Int(1));
  EXPECT_EQ(Run({"TTL", "k"}), Int(-1));
  EXPECT_TRUE(h.Cf(ColumnFamily::kExpire).empty());
  EXPECT_EQ(Run({"GET", "k"}), Bulk("v"));
}

TEST_F(ExpireCmds, ExpiredButStoredIsMissing) {  // Review Focus #1
  EXPECT_EQ(Run({"SET", "k", "v", "PX", "10"}), kOk);
  h.clock().Advance(10);
  EXPECT_EQ(Run({"TTL", "k"}), Int(-2));
  EXPECT_EQ(Run({"EXPIRE", "k", "100"}), Int(0));
  EXPECT_EQ(Run({"PERSIST", "k"}), Int(0));
  EXPECT_TRUE(h.Cf(ColumnFamily::kMeta).empty());  // the writers cleaned it up
}

TEST_F(ExpireCmds, WorksOnCollectionsToo) {
  encoding::Metadata m;
  m.type = encoding::ValueType::kHash;
  m.oid = 11;
  h.SeedMeta(0, "h", m);
  EXPECT_EQ(Run({"EXPIRE", "h", "5"}), Int(1));
  EXPECT_EQ(Run({"PTTL", "h"}), Int(5000));
  EXPECT_EQ(Run({"TYPE", "h"}), "+hash\r\n");
}

}  // namespace
}  // namespace tideskv
