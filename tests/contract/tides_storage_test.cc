#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "storage/tides_storage.h"
#include "support/temp_dir.h"

namespace tideskv::test {
namespace {

StorageOptions OptionsFor(const TempDir& dir, const std::string& name = "db") {
  StorageOptions o;
  o.path = (dir.path() / name).string();
  o.sync_mode = SyncMode::kNone;
  return o;
}

std::unique_ptr<TidesStorage> MustOpen(const StorageOptions& o) {
  auto s = TidesStorage::Open(o);
  EXPECT_TRUE(s.ok()) << s.status().ToString();
  return s.ok() ? std::move(s).value() : nullptr;
}

TEST(TidesStorage, SecondOpenOfSameDirectoryIsBusyAndSaysLocked) {
  TempDir dir;
  auto first = MustOpen(OptionsFor(dir));
  ASSERT_NE(first, nullptr);
  auto second = TidesStorage::Open(OptionsFor(dir));
  ASSERT_FALSE(second.ok());
  EXPECT_EQ(second.status().code(), StatusCode::kBusy);
  EXPECT_NE(second.status().message().find("locked"), std::string::npos);
}

TEST(TidesStorage, EveryColumnFamilySurvivesReopen) {
  TempDir dir;
  {
    auto s = MustOpen(OptionsFor(dir));
    ASSERT_NE(s, nullptr);
    auto txn = s->BeginWrite();
    ASSERT_TRUE(txn.ok());
    for (ColumnFamily cf : kAllColumnFamilies) {
      ASSERT_TRUE((*txn)->Put(cf, "k", std::string(ColumnFamilyName(cf))).ok());
    }
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  auto s = MustOpen(OptionsFor(dir));
  ASSERT_NE(s, nullptr);
  auto view = s->BeginRead();
  ASSERT_TRUE(view.ok());
  for (ColumnFamily cf : kAllColumnFamilies) {
    auto v = (*view)->Get(cf, "k");
    ASSERT_TRUE(v.ok()) << ColumnFamilyName(cf);
    EXPECT_EQ(*v, ColumnFamilyName(cf));
  }
}

TEST(TidesStorage, UncommittedWritesAreGoneAfterReopen) {
  TempDir dir;
  {
    auto s = MustOpen(OptionsFor(dir));
    ASSERT_NE(s, nullptr);
    auto txn = s->BeginWrite();
    ASSERT_TRUE(txn.ok());
    ASSERT_TRUE((*txn)->Put(ColumnFamily::kMeta, "k", "v").ok());
  }  // txn destroyed (rolled back) before the storage closes
  auto s = MustOpen(OptionsFor(dir));
  ASSERT_NE(s, nullptr);
  auto view = s->BeginRead();
  ASSERT_TRUE(view.ok());
  EXPECT_TRUE((*view)->Get(ColumnFamily::kMeta, "k").status().IsNotFound());
}

TEST(TidesStorage, BackupIsDirectlyOpenable) {
  TempDir dir;
  auto s = MustOpen(OptionsFor(dir));
  ASSERT_NE(s, nullptr);
  {
    auto txn = s->BeginWrite();
    ASSERT_TRUE(txn.ok());
    ASSERT_TRUE((*txn)->Put(ColumnFamily::kSubkey, "field", "value").ok());
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  const std::string backup_dir = (dir.path() / "backup").string();
  ASSERT_TRUE(s->Backup(backup_dir).ok());
  StorageOptions bo = OptionsFor(dir, "backup");
  auto copy = MustOpen(bo);
  ASSERT_NE(copy, nullptr);
  auto view = copy->BeginRead();
  ASSERT_TRUE(view.ok());
  auto v = (*view)->Get(ColumnFamily::kSubkey, "field");
  ASSERT_TRUE(v.ok());
  EXPECT_EQ(*v, "value");
}

TEST(TidesStorage, CheckpointSucceeds) {
  TempDir dir;
  auto s = MustOpen(OptionsFor(dir));
  ASSERT_NE(s, nullptr);
  EXPECT_TRUE(s->Checkpoint().ok());
}

StorageOptions OptionsWith(const TempDir& dir, Compression c) {
  StorageOptions o = OptionsFor(dir);
  o.compression = c;
  return o;
}

TEST(TidesStorageMaint, CompressionNoneIsAppliedAsNone) {
  TempDir dir;
  auto s = MustOpen(OptionsWith(dir, Compression::kNone));
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->compression(), Compression::kNone);
}

TEST(TidesStorageMaint, RequestedLz4IsAppliedOrFallsBackToNone) {
  TempDir dir;
  auto s = MustOpen(OptionsWith(dir, Compression::kLz4));
  ASSERT_NE(s, nullptr);
#ifdef TIDESKV_TEST_COMPRESSION
  EXPECT_EQ(s->compression(), Compression::kLz4);
#else
  EXPECT_EQ(s->compression(), Compression::kNone);  // build has no LZ4: warn + fall back, never fail
#endif
  auto txn = s->BeginWrite();
  ASSERT_TRUE(txn.ok());
  ASSERT_TRUE((*txn)->Put(ColumnFamily::kMeta, "k", std::string(4096, 'a')).ok());
  ASSERT_TRUE((*txn)->Commit().ok());
  ASSERT_TRUE(s->Checkpoint().ok());  // forces data through an sstable (and so the encoder)
  auto view = s->BeginRead();
  ASSERT_TRUE(view.ok());
  auto v = (*view)->Get(ColumnFamily::kMeta, "k");
  ASSERT_TRUE(v.ok());
  EXPECT_EQ(*v, std::string(4096, 'a'));
}

TEST(TidesStorageMaint, ReopenWithDifferentCompressionKeepsData) {
  TempDir dir;
  {
    auto s = MustOpen(OptionsWith(dir, Compression::kNone));
    ASSERT_NE(s, nullptr);
    auto txn = s->BeginWrite();
    ASSERT_TRUE((*txn)->Put(ColumnFamily::kMeta, "k", "v").ok());
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  auto s = MustOpen(OptionsWith(dir, Compression::kLz4));  // existing CFs get the new pipeline
  ASSERT_NE(s, nullptr);
  auto view = s->BeginRead();
  EXPECT_EQ(*(*view)->Get(ColumnFamily::kMeta, "k"), "v");
}

TEST(TidesStorageMaint, StatsCountKeysAndTombstonesAfterFlush) {
  TempDir dir;
  auto s = MustOpen(OptionsWith(dir, Compression::kNone));
  ASSERT_NE(s, nullptr);
  {
    auto txn = s->BeginWrite();
    for (int i = 0; i < 100; ++i) {
      ASSERT_TRUE((*txn)->Put(ColumnFamily::kMeta, "k" + std::to_string(i), "v").ok());
    }
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  {
    auto txn = s->BeginWrite();
    for (int i = 0; i < 10; ++i)
      ASSERT_TRUE((*txn)->Delete(ColumnFamily::kMeta, "k" + std::to_string(i)).ok());
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  ASSERT_TRUE(s->Checkpoint().ok());
  auto stats = s->Stats();
  ASSERT_TRUE(stats.ok()) << stats.status().ToString();
  EXPECT_GE(stats->flush_count, 1u);
  EXPECT_GT(stats->cf[CfIndex(ColumnFamily::kMeta)].total_keys, 0u);
  EXPECT_GT(stats->cf[CfIndex(ColumnFamily::kMeta)].total_tombstones, 0u);
}

TEST(TidesStorageMaint, CompactRangeKeepsLiveDataAndRejectsBadBounds) {
  TempDir dir;
  auto s = MustOpen(OptionsWith(dir, Compression::kNone));
  ASSERT_NE(s, nullptr);
  {
    auto txn = s->BeginWrite();
    ASSERT_TRUE((*txn)->Put(ColumnFamily::kSubkey, "a1", "x").ok());
    ASSERT_TRUE((*txn)->Put(ColumnFamily::kSubkey, "b1", "y").ok());
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  {
    auto txn = s->BeginWrite();
    ASSERT_TRUE((*txn)->DeletePrefix(ColumnFamily::kSubkey, "a").ok());
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  ASSERT_TRUE(s->Checkpoint().ok());
  // A background compaction may hold the CF: Busy is a legal answer, retry a few times.
  auto compact = [&](std::string_view lo, std::string_view hi) {
    Status st = s->CompactRange(ColumnFamily::kSubkey, lo, hi);
    for (int i = 0; i < 20 && st.code() == StatusCode::kBusy; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      st = s->CompactRange(ColumnFamily::kSubkey, lo, hi);
    }
    return st;
  };
  EXPECT_TRUE(compact("a", "b").ok());
  EXPECT_TRUE(compact("", "").ok());  // whole CF
  EXPECT_EQ(s->CompactRange(ColumnFamily::kSubkey, "b", "a").code(), StatusCode::kInvalidArgument);
  auto view = s->BeginRead();
  EXPECT_TRUE((*view)->Get(ColumnFamily::kSubkey, "a1").status().IsNotFound());
  EXPECT_EQ(*(*view)->Get(ColumnFamily::kSubkey, "b1"), "y");
}

}  // namespace
}  // namespace tideskv::test
