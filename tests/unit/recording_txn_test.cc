#include "exec/recording_txn.h"

#include <gtest/gtest.h>

#include "encoding/key_codec.h"
#include "encoding/replog_codec.h"
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

TEST(RecordingTxn, ReleaseKeepsTheCountsAndForgetsTheSavepoint) {
  InMemoryStorage s;
  auto inner = s.BeginWrite();
  RecordingTxn txn(**inner);
  ASSERT_TRUE(txn.Savepoint("outer").ok());
  ASSERT_TRUE(txn.Put(ColumnFamily::kMeta, encoding::MetaKeyFor(0, "a"), "v").ok());
  ASSERT_TRUE(txn.Savepoint("sp").ok());
  ASSERT_TRUE(txn.Put(ColumnFamily::kMeta, encoding::MetaKeyFor(0, "b"), "v").ok());
  ASSERT_TRUE(txn.DeletePrefix(ColumnFamily::kSubkey, "p").ok());
  ASSERT_TRUE(txn.ReleaseSavepoint("sp").ok());
  EXPECT_EQ(txn.writes(), 3u);
  EXPECT_EQ(txn.meta_keys(), (std::vector<DbKey>{{0, "a"}, {0, "b"}}));
  EXPECT_EQ(txn.deletes().size(), 1u);
  EXPECT_TRUE(txn.RollbackToSavepoint("sp").IsNotFound());
  EXPECT_TRUE(txn.ReleaseSavepoint("missing").IsNotFound());
  ASSERT_TRUE(txn.RollbackToSavepoint("outer").ok());  // the earlier savepoint still undoes everything
  EXPECT_EQ(txn.writes(), 0u);
  EXPECT_TRUE(txn.meta_keys().empty());
  EXPECT_TRUE(txn.deletes().empty());
}

using encoding::ReplOp;
using encoding::ReplOpType;
using Ops = std::vector<ReplOp>;

TEST(RecordingTxn, CapturesOpsInProgramOrderWhenEnabled) {
  InMemoryStorage s;
  auto inner = s.BeginWrite();
  RecordingTxn rec(**inner);
  rec.CaptureOps(true);
  ASSERT_TRUE(rec.Put(ColumnFamily::kMeta, "k", "v").ok());
  ASSERT_TRUE(rec.Delete(ColumnFamily::kSubkey, "s").ok());
  ASSERT_TRUE(rec.DeletePrefix(ColumnFamily::kZScore, "p").ok());
  ASSERT_TRUE(rec.DeleteRange(ColumnFamily::kExpire, "a", "b").ok());
  EXPECT_EQ(rec.ops(), (Ops{{ReplOpType::kPut, ColumnFamily::kMeta, "k", "v"},
                            {ReplOpType::kDelete, ColumnFamily::kSubkey, "s", ""},
                            {ReplOpType::kDeletePrefix, ColumnFamily::kZScore, "p", ""},
                            {ReplOpType::kDeleteRange, ColumnFamily::kExpire, "a", "b"}}));
}

TEST(RecordingTxn, CapturesNothingWhenDisabled) {
  InMemoryStorage s;
  auto inner = s.BeginWrite();
  RecordingTxn rec(**inner);
  ASSERT_TRUE(rec.Put(ColumnFamily::kMeta, "k", "v").ok());
  ASSERT_TRUE(rec.Delete(ColumnFamily::kSubkey, "s").ok());
  ASSERT_TRUE(rec.DeletePrefix(ColumnFamily::kZScore, "p").ok());
  ASSERT_TRUE(rec.DeleteRange(ColumnFamily::kExpire, "a", "b").ok());
  EXPECT_TRUE(rec.ops().empty());
}

TEST(RecordingTxn, RollbackToSavepointDropsLaterOps) {
  InMemoryStorage s;
  auto inner = s.BeginWrite();
  RecordingTxn rec(**inner);
  rec.CaptureOps(true);
  ASSERT_TRUE(rec.Put(ColumnFamily::kMeta, "a", "1").ok());
  ASSERT_TRUE(rec.Savepoint("s").ok());
  ASSERT_TRUE(rec.Put(ColumnFamily::kMeta, "b", "2").ok());
  ASSERT_TRUE(rec.Delete(ColumnFamily::kMeta, "c").ok());
  ASSERT_TRUE(rec.RollbackToSavepoint("s").ok());
  EXPECT_EQ(rec.ops(), (Ops{{ReplOpType::kPut, ColumnFamily::kMeta, "a", "1"}}));
  ASSERT_TRUE(rec.Put(ColumnFamily::kMeta, "d", "4").ok());
  EXPECT_EQ(rec.ops(), (Ops{{ReplOpType::kPut, ColumnFamily::kMeta, "a", "1"},
                            {ReplOpType::kPut, ColumnFamily::kMeta, "d", "4"}}));
}

TEST(RecordingTxn, ReleaseSavepointKeepsOps) {
  InMemoryStorage s;
  auto inner = s.BeginWrite();
  RecordingTxn rec(**inner);
  rec.CaptureOps(true);
  ASSERT_TRUE(rec.Put(ColumnFamily::kMeta, "a", "1").ok());
  ASSERT_TRUE(rec.Savepoint("s").ok());
  ASSERT_TRUE(rec.Put(ColumnFamily::kMeta, "b", "2").ok());
  ASSERT_TRUE(rec.ReleaseSavepoint("s").ok());
  EXPECT_EQ(rec.ops(), (Ops{{ReplOpType::kPut, ColumnFamily::kMeta, "a", "1"},
                            {ReplOpType::kPut, ColumnFamily::kMeta, "b", "2"}}));
}

TEST(RecordingTxn, NestedSavepointsTruncateToTheRightMark) {
  InMemoryStorage s;
  auto inner = s.BeginWrite();
  RecordingTxn rec(**inner);
  rec.CaptureOps(true);
  ASSERT_TRUE(rec.Put(ColumnFamily::kMeta, "a", "1").ok());
  ASSERT_TRUE(rec.Savepoint("s1").ok());
  ASSERT_TRUE(rec.Put(ColumnFamily::kMeta, "b", "2").ok());
  ASSERT_TRUE(rec.Savepoint("s2").ok());
  ASSERT_TRUE(rec.Put(ColumnFamily::kMeta, "c", "3").ok());
  ASSERT_TRUE(rec.RollbackToSavepoint("s1").ok());
  const Ops just_a{{ReplOpType::kPut, ColumnFamily::kMeta, "a", "1"}};
  EXPECT_EQ(rec.ops(), just_a);
  ASSERT_TRUE(rec.RollbackToSavepoint("s1").ok());  // s1 survives its own rollback
  EXPECT_EQ(rec.ops(), just_a);
}

TEST(RecordingTxn, FullRollbackClearsOps) {
  InMemoryStorage s;
  auto inner = s.BeginWrite();
  RecordingTxn rec(**inner);
  rec.CaptureOps(true);
  ASSERT_TRUE(rec.Put(ColumnFamily::kMeta, "a", "1").ok());
  ASSERT_TRUE(rec.Rollback().ok());
  EXPECT_TRUE(rec.ops().empty());
}

TEST(RecordingTxn, FailedInnerCallCapturesNothing) {
  InMemoryStorage s;
  auto inner = s.BeginWrite();
  RecordingTxn rec(**inner);
  rec.CaptureOps(true);
  EXPECT_FALSE(rec.Put(ColumnFamily::kMeta, "", "v").ok());
  EXPECT_FALSE(rec.DeletePrefix(ColumnFamily::kSubkey, "").ok());
  EXPECT_TRUE(rec.ops().empty());
}

TEST(RecordingTxn, RefusesReplogWrites) {
  InMemoryStorage s;
  auto inner = s.BeginWrite();
  RecordingTxn rec(**inner);
  rec.CaptureOps(true);
  const auto cf = ColumnFamily::kReplog;
  EXPECT_EQ(rec.Put(cf, encoding::ReplogKey(1), "x").code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(rec.Delete(cf, encoding::ReplogKey(1)).code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(rec.DeletePrefix(cf, "p").code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(rec.DeleteRange(cf, encoding::ReplogKey(1), encoding::ReplogKey(2)).code(),
            StatusCode::kInvalidArgument);
  EXPECT_TRUE(rec.ops().empty());
  EXPECT_EQ(rec.writes(), 0u);
}

}  // namespace
}  // namespace tideskv
