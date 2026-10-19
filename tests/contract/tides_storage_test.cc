#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

#include "encoding/key_codec.h"
#include "exec/engine.h"
#include "exec/lock_manager.h"
#include "storage/tides_storage.h"
#include "support/temp_dir.h"

extern "C" {
#include <db.h>
}

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
    ASSERT_TRUE(s->Checkpoint().ok());
  }
  auto s = MustOpen(OptionsWith(dir, Compression::kLz4));  // existing CFs get the new pipeline
  ASSERT_NE(s, nullptr);
  auto view = s->BeginRead();
  EXPECT_EQ(*(*view)->Get(ColumnFamily::kMeta, "k"), "v");
}

TEST(TidesStorageMaint, ReopenWithSameCompressionKeepsData) {
  TempDir dir;
  for (int round = 0; round < 2; ++round) {
    auto s = MustOpen(OptionsWith(dir, Compression::kLz4));
    ASSERT_NE(s, nullptr);
    if (round == 0) {
      auto txn = s->BeginWrite();
      ASSERT_TRUE((*txn)->Put(ColumnFamily::kMeta, "k", "v").ok());
      ASSERT_TRUE((*txn)->Commit().ok());
      ASSERT_TRUE(s->Checkpoint().ok());
    } else {
      auto view = s->BeginRead();
      EXPECT_EQ(*(*view)->Get(ColumnFamily::kMeta, "k"), "v");
    }
  }
}

// TidesDB 10.1.1's default dividing_level_offset (1) can rewrite an over-capacity level into itself
// forever, compacting with no writes at all; TidesKV sets 0 on every family, existing ones included.
TEST(TidesStorageMaint, EveryFamilyUsesDividingLevelOffsetZero) {
  TempDir dir;
  StorageOptions o = OptionsFor(dir);
  {  // a family created by an older TidesKV, with TidesDB's default offset
    tidesdb_config_t cfg = tidesdb_default_config();
    cfg.db_path = o.path.data();
    tidesdb_t* db = nullptr;
    ASSERT_EQ(tidesdb_open(&cfg, &db), TDB_SUCCESS);
    tidesdb_column_family_config_t cf = tidesdb_default_column_family_config();
    ASSERT_EQ(cf.dividing_level_offset, 1);
    const std::string name(ColumnFamilyName(ColumnFamily::kMeta));
    ASSERT_EQ(tidesdb_create_column_family(db, name.c_str(), &cf), TDB_SUCCESS);
    ASSERT_EQ(tidesdb_close(db), TDB_SUCCESS);
  }
  auto s = MustOpen(o);
  ASSERT_NE(s, nullptr);
  auto st = s->Stats();
  ASSERT_TRUE(st.ok());
  for (const CfStats& cf : st->cf) EXPECT_EQ(cf.dividing_level_offset, 0);
}

// M8: l1_file_count_trigger reaches every column family, new ones at creation and existing ones on
// reopen; 0 leaves TidesDB's default (4).
TEST(TidesStorageMaint, L1FileCountTriggerAppliesToNewAndExistingFamilies) {
  TempDir dir;
  StorageOptions o = OptionsFor(dir);
  {
    o.l1_file_count_trigger = 12;
    o.l0_stall_threshold = 48;
    auto s = MustOpen(o);
    ASSERT_NE(s, nullptr);
    auto st = s->Stats();
    ASSERT_TRUE(st.ok());
    for (const CfStats& cf : st->cf) EXPECT_EQ(cf.l1_file_count_trigger, 12);
  }
  {
    o.l1_file_count_trigger = 20;
    auto s = MustOpen(o);
    ASSERT_NE(s, nullptr);
    auto st = s->Stats();
    ASSERT_TRUE(st.ok());
    for (const CfStats& cf : st->cf) EXPECT_EQ(cf.l1_file_count_trigger, 20);
  }
  TempDir fresh;
  auto s = MustOpen(OptionsFor(fresh));
  ASSERT_NE(s, nullptr);
  auto st = s->Stats();
  ASSERT_TRUE(st.ok());
  for (const CfStats& cf : st->cf) EXPECT_EQ(cf.l1_file_count_trigger, 4);
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

TEST(TidesStorageMaint, CompactKeepsLiveDataAndDropsDeletedData) {
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
  Status st = s->Compact(ColumnFamily::kSubkey);
  for (int i = 0; i < 20 && st.code() == StatusCode::kBusy; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    st = s->Compact(ColumnFamily::kSubkey);
  }
  EXPECT_TRUE(st.ok()) << st.ToString();
  auto view = s->BeginRead();
  EXPECT_TRUE((*view)->Get(ColumnFamily::kSubkey, "a1").status().IsNotFound());
  EXPECT_EQ(*(*view)->Get(ColumnFamily::kSubkey, "b1"), "y");
}

// A TidesDB transaction must hold 1000 savepoints and roll back to arbitrary ones (M5a G1).
TEST(TidesStorage, BatchOfThousandSavepoints) {
  TempDir dir;
  auto s = MustOpen(OptionsFor(dir));
  ASSERT_NE(s, nullptr);
  LockManager locks;
  SystemClock clock;
  Engine engine(*s, locks, clock);
  auto get = [&](const std::string& k) {
    std::string out = "<missing>";
    EXPECT_TRUE(engine
                    .Read([&](ReadView& v, int64_t) {
                      auto r = v.Get(ColumnFamily::kMeta, encoding::MetaKeyFor(0, k));
                      if (r.ok()) out = *r;
                      return r.ok() || r.status().IsNotFound() ? Status::Ok() : r.status();
                    })
                    .ok());
    return out;
  };
  auto batch = engine.BeginBatch();
  ASSERT_TRUE(batch.ok());
  for (int i = 0; i < 1000; ++i) {
    const std::string k = "k" + std::to_string(i);
    const Status st = engine.Write(0, {k}, [&](WriteTxn& t, int64_t) {
      EXPECT_TRUE(t.Put(ColumnFamily::kMeta, encoding::MetaKeyFor(0, k), "v").ok());
      return i % 3 == 0 ? Status::UserError("ERR x") : Status::Ok();
    });
    EXPECT_EQ(st.ok(), i % 3 != 0);
  }
  ASSERT_TRUE((*batch)->Commit().ok());
  batch->reset();
  EXPECT_EQ(get("k0"), "<missing>");
  EXPECT_EQ(get("k1"), "v");
  EXPECT_EQ(get("k999"), "<missing>");
  EXPECT_EQ(get("k998"), "v");
}

// Seconds for `writes` single-put writes (INCR-like, two alternating keys) in one batch, which is then
// rolled back. Best of three runs, to damp scheduler noise.
double BatchWriteSeconds(Engine& engine, int writes) {
  double best = 1e9;
  for (int run = 0; run < 3; ++run) {
    auto batch = engine.BeginBatch();
    EXPECT_TRUE(batch.ok());
    if (!batch.ok()) return best;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < writes; ++i) {
      const std::string k = i % 2 == 0 ? "a" : "b";
      const Status st = engine.Write(0, {k}, [&](WriteTxn& t, int64_t) {
        return t.Put(ColumnFamily::kMeta, encoding::MetaKeyFor(0, k), std::to_string(i));
      });
      EXPECT_TRUE(st.ok()) << st.ToString();
    }
    best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
  }  // each batch is destroyed uncommitted: rolled back
  return best;
}

// Task 13: every write in a batch takes a savepoint, and TidesDB finds savepoints by name with a linear
// scan, so unreleased savepoints made a batch O(N^2) in its writes (~16x for 4x the writes). The batch
// must release them: 4x the writes must cost well under 8x the time. A ratio is build-independent, so the
// same bound holds under debug and the sanitizers; best-of-3 keeps scheduler noise out of it.
TEST(TidesStorage, BatchWriteCostIsLinear) {
  TempDir dir;
  auto s = MustOpen(OptionsFor(dir));
  ASSERT_NE(s, nullptr);
  LockManager locks;
  SystemClock clock;
  Engine engine(*s, locks, clock);
  const double small = BatchWriteSeconds(engine, 4000);
  const double large = BatchWriteSeconds(engine, 16000);
  EXPECT_LT(large, 8 * small) << "4000 writes: " << small << " s, 16000 writes: " << large << " s";
}

}  // namespace
}  // namespace tideskv::test
