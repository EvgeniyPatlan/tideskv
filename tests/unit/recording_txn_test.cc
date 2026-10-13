#include "exec/recording_txn.h"

#include <gtest/gtest.h>

#include "encoding/key_codec.h"
#include "storage/memory_storage.h"

namespace tideskv {
namespace {

TEST(RecordingTxn, RecordsSuccessfulRangeDeletesOnly) {
  InMemoryStorage s;
  auto inner = s.BeginWrite();
  RecordingTxn txn(**inner);
  ASSERT_TRUE(txn.Put(ColumnFamily::kMeta, "k", "v").ok());
  ASSERT_TRUE(txn.DeletePrefix(ColumnFamily::kSubkey, "pfx").ok());
  ASSERT_TRUE(txn.DeleteRange(ColumnFamily::kMeta, "a", "b").ok());
  EXPECT_FALSE(txn.DeletePrefix(ColumnFamily::kSubkey, "").ok());  // invalid: not recorded
  ASSERT_EQ(txn.deletes().size(), 2u);
  EXPECT_TRUE(txn.deletes()[0].is_prefix);
  EXPECT_EQ(txn.deletes()[0].lo, "pfx");
  EXPECT_FALSE(txn.deletes()[1].is_prefix);
  EXPECT_EQ(txn.deletes()[1].hi, "b");
  ASSERT_TRUE(txn.Commit().ok());
  auto view = s.BeginRead();
  EXPECT_EQ(*(*view)->Get(ColumnFamily::kMeta, "k"), "v");  // forwarded to the real txn
}

TEST(RecordingTxn, SavepointRollbackForgetsLaterDeletes) {
  InMemoryStorage s;
  auto inner = s.BeginWrite();
  RecordingTxn txn(**inner);
  ASSERT_TRUE(txn.DeletePrefix(ColumnFamily::kSubkey, "one").ok());
  ASSERT_TRUE(txn.Savepoint("sp").ok());
  ASSERT_TRUE(txn.DeletePrefix(ColumnFamily::kSubkey, "two").ok());
  ASSERT_TRUE(txn.RollbackToSavepoint("sp").ok());
  ASSERT_EQ(txn.deletes().size(), 1u);
  EXPECT_EQ(txn.deletes()[0].lo, "one");
  ASSERT_TRUE(txn.Rollback().ok());
  EXPECT_TRUE(txn.deletes().empty());
}

TEST(RecordingTxn, CountsWritesAndDecodesMetaKeys) {
  InMemoryStorage s;
  auto inner = s.BeginWrite();
  RecordingTxn txn(**inner);
  EXPECT_EQ(txn.writes(), 0u);
  ASSERT_TRUE(txn.Put(ColumnFamily::kMeta, encoding::MetaKeyFor(3, "user"), "v").ok());
  ASSERT_TRUE(txn.Delete(ColumnFamily::kMeta, encoding::MetaKeyFor(0, "gone")).ok());
  ASSERT_TRUE(txn.Put(ColumnFamily::kSubkey, "opaque", "v").ok());  // counted, not decoded
  ASSERT_TRUE(txn.Put(ColumnFamily::kMeta, "\x01", "v").ok());      // too short to decode: counted only
  EXPECT_EQ(txn.writes(), 4u);
  EXPECT_EQ(txn.meta_keys(), (std::vector<DbKey>{{3, "user"}, {0, "gone"}}));
}

TEST(RecordingTxn, SavepointRollbackForgetsLaterWritesAndKeys) {
  InMemoryStorage s;
  auto inner = s.BeginWrite();
  RecordingTxn txn(**inner);
  ASSERT_TRUE(txn.Put(ColumnFamily::kMeta, encoding::MetaKeyFor(0, "a"), "v").ok());
  ASSERT_TRUE(txn.Savepoint("sp").ok());
  ASSERT_TRUE(txn.Put(ColumnFamily::kMeta, encoding::MetaKeyFor(0, "b"), "v").ok());
  ASSERT_TRUE(txn.DeletePrefix(ColumnFamily::kSubkey, "p").ok());
  ASSERT_TRUE(txn.RollbackToSavepoint("sp").ok());
  EXPECT_EQ(txn.writes(), 1u);
  EXPECT_EQ(txn.meta_keys(), (std::vector<DbKey>{{0, "a"}}));
}

}  // namespace
}  // namespace tideskv
