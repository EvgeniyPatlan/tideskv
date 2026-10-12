#include <gtest/gtest.h>

#include "commands/reply_util.h"
#include "support/command_harness.h"
#include "types/format_version.h"

namespace tideskv {
namespace {

using test::Bulk;
using test::CommandHarness;
using test::Err;
using test::Int;
using test::kNil;
using test::kOk;

struct KeyspaceCmds : ::testing::Test {
  CommandHarness h;
};

TEST_F(KeyspaceCmds, DelAndUnlink) {
  EXPECT_EQ(h.Run({"MSET", "a", "1", "b", "2", "c", "3"}), kOk);
  EXPECT_EQ(h.Run({"DEL", "a", "a", "nope"}), Int(1));
  EXPECT_EQ(h.Run({"UNLINK", "b", "c"}), Int(2));
  EXPECT_EQ(h.Run({"GET", "b"}), kNil);
  EXPECT_EQ(h.Run({"DEL"}), Err("ERR wrong number of arguments for 'del' command"));
}

TEST_F(KeyspaceCmds, DelDropsCollectionData) {
  h.SeedHash(0, "h", 9, true);
  EXPECT_EQ(h.Run({"DEL", "h"}), Int(1));
  EXPECT_TRUE(h.Cf(ColumnFamily::kSubkey).empty());
}

TEST_F(KeyspaceCmds, ExistsCountsDuplicates) {
  EXPECT_EQ(h.Run({"SET", "a", "1"}), kOk);
  EXPECT_EQ(h.Run({"EXISTS", "a", "a", "b"}), Int(2));
  EXPECT_EQ(h.Run({"EXISTS", "b"}), Int(0));
}

TEST_F(KeyspaceCmds, Type) {
  EXPECT_EQ(h.Run({"SET", "s", "1"}), kOk);
  h.SeedHash(0, "h", 4, true);
  EXPECT_EQ(h.Run({"TYPE", "s"}), "+string\r\n");
  EXPECT_EQ(h.Run({"TYPE", "h"}), "+hash\r\n");
  EXPECT_EQ(h.Run({"TYPE", "nope"}), "+none\r\n");
}

TEST_F(KeyspaceCmds, ExpiredButStoredIsAbsent) {
  EXPECT_EQ(h.Run({"SET", "k", "v", "PX", "10"}), kOk);
  h.clock().Advance(10);
  EXPECT_EQ(h.Run({"EXISTS", "k"}), Int(0));
  EXPECT_EQ(h.Run({"TYPE", "k"}), "+none\r\n");
  EXPECT_EQ(h.Run({"DBSIZE"}), Int(0));
  EXPECT_EQ(h.Run({"DEL", "k"}), Int(0));
  EXPECT_TRUE(h.Cf(ColumnFamily::kMeta).empty());  // DEL still cleaned it up
  EXPECT_TRUE(h.Cf(ColumnFamily::kExpire).empty());
}

TEST_F(KeyspaceCmds, Select) {
  EXPECT_EQ(h.Run({"SELECT", "15"}), kOk);
  EXPECT_EQ(h.session.db, 15);
  EXPECT_EQ(h.Run({"SELECT", "16"}), Err(kErrDbIndexOutOfRange));
  EXPECT_EQ(h.Run({"SELECT", "-1"}), Err(kErrDbIndexOutOfRange));
  EXPECT_EQ(h.Run({"SELECT", "one"}), Err(kErrNotInteger));
  EXPECT_EQ(h.Run({"SELECT", "99999999999"}), Err(kErrNotInteger));  // does not fit int32
  EXPECT_EQ(h.Run({"SELECT", "2147483647"}), Err(kErrDbIndexOutOfRange));
  EXPECT_EQ(h.session.db, 15);  // unchanged by errors
}

TEST_F(KeyspaceCmds, DbSizeIsPerDb) {
  EXPECT_EQ(h.Run({"MSET", "a", "1", "b", "2"}), kOk);
  EXPECT_EQ(h.Run({"DBSIZE"}), Int(2));
  h.session.db = 1;
  EXPECT_EQ(h.Run({"DBSIZE"}), Int(0));
  EXPECT_EQ(h.Run({"SET", "x", "1"}), kOk);
  EXPECT_EQ(h.Run({"DBSIZE"}), Int(1));
}

TEST_F(KeyspaceCmds, FlushDbClearsOnlyTheCurrentDb) {
  EXPECT_EQ(h.Run({"SET", "a", "1", "EX", "100"}), kOk);
  h.SeedHash(0, "h", 1, true);
  h.session.db = 1;
  EXPECT_EQ(h.Run({"SET", "a", "one"}), kOk);
  h.SeedHash(1, "h", 2, true);
  h.session.db = 0;
  EXPECT_EQ(h.Run({"FLUSHDB"}), kOk);
  EXPECT_EQ(h.Run({"DBSIZE"}), Int(0));
  EXPECT_TRUE(h.Cf(ColumnFamily::kExpire).empty());
  EXPECT_EQ(h.Cf(ColumnFamily::kSubkey).size(), 1u);  // db 1's field survives
  h.session.db = 1;
  EXPECT_EQ(h.Run({"GET", "a"}), Bulk("one"));
  EXPECT_EQ(h.Run({"FLUSHDB", "ASYNC"}), kOk);
  EXPECT_EQ(h.Run({"FLUSHDB", "sync"}), kOk);
  EXPECT_EQ(h.Run({"FLUSHDB", "LATER"}), Err(kErrSyntax));
}

TEST_F(KeyspaceCmds, FlushAllClearsEveryDbButNotSys) {
  ASSERT_TRUE(EnsureFormatVersion(h.services.storage_).ok());
  EXPECT_EQ(h.Run({"SET", "a", "1"}), kOk);
  h.session.db = 15;
  EXPECT_EQ(h.Run({"SET", "z", "1", "PX", "100"}), kOk);
  h.SeedHash(15, "h", 3, true);
  EXPECT_EQ(h.Run({"FLUSHALL"}), kOk);
  for (auto cf : {ColumnFamily::kMeta, ColumnFamily::kSubkey, ColumnFamily::kZScore, ColumnFamily::kExpire}) {
    EXPECT_TRUE(h.Cf(cf).empty()) << ColumnFamilyName(cf);
  }
  EXPECT_EQ(h.Cf(ColumnFamily::kSys).count(std::string(kFormatVersionKey)), 1u);
  EXPECT_EQ(h.Run({"FLUSHALL", "ASYNC"}), kOk);
  EXPECT_EQ(h.Run({"FLUSHALL", "x"}), Err(kErrSyntax));
}

}  // namespace
}  // namespace tideskv
