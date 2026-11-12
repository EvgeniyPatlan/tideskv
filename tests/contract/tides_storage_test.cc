#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

// TidesDB holds every in-flight commit's range deletes in one 32-slot table and a full table
// conflicts, so overlapping commits of up to kMaxNativeRangeDeletes ranges each must not fail.
TEST(TidesStorage, ConcurrentRangeDeleteCommitsAllSucceed) {
  constexpr int kThreads = 4;
  constexpr int kTxns = 50;
  constexpr int kRanges = 30;
  TempDir dir;
  auto s = MustOpen(OptionsFor(dir));
  ASSERT_NE(s, nullptr);
  std::atomic<int> ok{0};
  std::vector<std::string> failures(kThreads);
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kTxns; ++i) {
        auto txn = s->BeginWrite();
        if (!txn.ok()) {
          failures[t] = txn.status().ToString();
          return;
        }
        for (int k = 0; k < kRanges; ++k) {
          const std::string prefix = std::to_string(t) + ":" + std::to_string(i) + ":" + std::to_string(k);
          if (Status st = (*txn)->DeletePrefix(ColumnFamily::kSubkey, prefix); !st.ok()) {
            failures[t] = st.ToString();
            return;
          }
        }
        if (Status st = (*txn)->Put(ColumnFamily::kMeta, std::to_string(t) + ":" + std::to_string(i), "v");
            !st.ok()) {
          failures[t] = st.ToString();
          return;
        }
        if (Status st = (*txn)->Commit(); !st.ok()) {
          failures[t] = "commit " + std::to_string(i) + ": " + st.ToString();
          return;
        }
        ok.fetch_add(1);
      }
    });
  }
  for (auto& th : threads) th.join();
  for (int t = 0; t < kThreads; ++t) EXPECT_EQ(failures[t], "") << "thread " << t;
  EXPECT_EQ(ok.load(), kThreads * kTxns);
}

// A checkpoint rotates the memtable and waits for the flush queue; writers keep committing all along.
TEST(TidesStorage, CheckpointSucceedsUnderConcurrentCommits) {
  constexpr int kThreads = 4;
  constexpr int kCheckpoints = 20;
  TempDir dir;
  StorageOptions o = OptionsFor(dir);
  o.memtable_size = 256 * 1024;
  auto s = MustOpen(o);
  ASSERT_NE(s, nullptr);
  std::atomic<bool> stop{false};
  std::atomic<int> commits{0};
  std::vector<std::string> failures(kThreads);
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      const std::string value(1024, 'v');
      for (int i = 0; !stop.load(); ++i) {
        auto txn = s->BeginWrite();
        if (!txn.ok()) {
          failures[t] = txn.status().ToString();
          return;
        }
        if (Status st = (*txn)->Put(ColumnFamily::kMeta, std::to_string(t) + ":" + std::to_string(i), value);
            !st.ok()) {
          failures[t] = st.ToString();
          return;
        }
        if (Status st = (*txn)->Commit(); !st.ok()) {
          failures[t] = "commit " + std::to_string(i) + ": " + st.ToString();
          return;
        }
        commits.fetch_add(1);
      }
    });
  }
  std::vector<std::string> checkpoint_failures;
  for (int c = 0; c < kCheckpoints; ++c) {
    if (Status st = s->Checkpoint(); !st.ok()) {
      checkpoint_failures.push_back(std::to_string(c) + ": " + st.ToString());
    }
  }
  stop.store(true);
  for (auto& th : threads) th.join();
  EXPECT_TRUE(checkpoint_failures.empty()) << checkpoint_failures.front();
  for (int t = 0; t < kThreads; ++t) EXPECT_EQ(failures[t], "") << "thread " << t;
  EXPECT_GT(commits.load(), 0);
}

// Checkpoints against sustained writers into every family, a 64 KiB memtable and a single flush
// thread, so rotations queue memtables throughout. A checkpoint waits for what was queued when it was
// called, not for an empty queue, so it returns ok well inside TidesDB's 60 s backstop. In process
// the flush thread still keeps up, so the queue does empty now and then; the regime where it never
// does is pinned down by TidesDB's own flush wait suite (tidesdb-flush-wait-called-generations.patch),
// which throttles flush directly.
TEST(TidesStorage, CheckpointReturnsPromptlyUnderSustainedCommits) {
  constexpr int kThreads = 8;
  constexpr auto kCheckpointFor = std::chrono::seconds(1);
  constexpr auto kBound = std::chrono::seconds(10);
  TempDir dir;
  StorageOptions o = OptionsFor(dir);
  o.memtable_size = 64 * 1024;
  o.flush_threads = 1;
  o.l0_stall_threshold = 64;  // TidesKV's own limit
  auto s = MustOpen(o);
  ASSERT_NE(s, nullptr);
  std::atomic<bool> stop{false};
  std::atomic<int> commits{0};
  std::vector<std::string> failures(kThreads);
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      const std::string value(1024, 'v');
      for (int i = 0; !stop.load(); ++i) {
        auto txn = s->BeginWrite();
        if (!txn.ok()) {
          failures[t] = txn.status().ToString();
          return;
        }
        for (ColumnFamily cf : kAllColumnFamilies) {
          if (Status st = (*txn)->Put(cf, std::to_string(t) + ":" + std::to_string(i), value); !st.ok()) {
            failures[t] = st.ToString();
            return;
          }
        }
        if (Status st = (*txn)->Commit(); !st.ok()) {
          failures[t] = "commit " + std::to_string(i) + ": " + st.ToString();
          return;
        }
        commits.fetch_add(1);
      }
    });
  }
  // writers first, so the checkpoints meet a queue that is being refilled
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (commits.load() < 1000 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::vector<std::string> checkpoint_failures;
  int checkpoints = 0;
  const auto until = std::chrono::steady_clock::now() + kCheckpointFor;
  while (std::chrono::steady_clock::now() < until) {
    const auto start = std::chrono::steady_clock::now();
    const Status st = s->Checkpoint();
    const auto took = std::chrono::steady_clock::now() - start;
    if (!st.ok() || took >= kBound) {
      checkpoint_failures.push_back(
          std::to_string(checkpoints) + ": " + st.ToString() + " after " +
          std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()) + " ms");
    }
    ++checkpoints;
  }
  stop.store(true);
  for (auto& th : threads) th.join();
  EXPECT_GT(checkpoints, 0);
  EXPECT_TRUE(checkpoint_failures.empty()) << checkpoint_failures.front();
  for (int t = 0; t < kThreads; ++t) EXPECT_EQ(failures[t], "") << "thread " << t;
  EXPECT_GE(commits.load(), 1000);
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

// The stats carry the block cache's charged bytes and entries: none on a fresh store, some once a read
// decodes a flushed node.
TEST(TidesStorageMaint, StatsReportBlockCacheUse) {
  TempDir dir;
  auto s = MustOpen(OptionsFor(dir));
  ASSERT_NE(s, nullptr);
  auto before = s->Stats();
  ASSERT_TRUE(before.ok());
  EXPECT_EQ(before->block_cache_entries, 0u);
  EXPECT_EQ(before->block_cache_bytes, 0u);
  {
    auto txn = s->BeginWrite();
    ASSERT_TRUE(txn.ok());
    for (int i = 0; i < 1000; ++i) {
      ASSERT_TRUE((*txn)->Put(ColumnFamily::kMeta, "k" + std::to_string(i), std::string(100, 'v')).ok());
    }
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  ASSERT_TRUE(s->Checkpoint().ok());
  auto view = s->BeginRead();
  ASSERT_TRUE(view.ok());
  for (int i = 0; i < 1000; i += 97)
    ASSERT_TRUE((*view)->Get(ColumnFamily::kMeta, "k" + std::to_string(i)).ok());
  auto after = s->Stats();
  ASSERT_TRUE(after.ok());
  EXPECT_GT(after->block_cache_entries, 0u);
  EXPECT_GT(after->block_cache_bytes, 0u);
}

// An iterator with fill_cache = false reads flushed data without adding to the block cache, and sees what a
// cached iterator sees (which does fill it).
TEST(TidesStorageMaint, IteratorWithoutCacheFillLeavesBlockCache) {
  TempDir dir;
  auto s = MustOpen(OptionsFor(dir));
  ASSERT_NE(s, nullptr);
  {
    auto txn = s->BeginWrite();
    ASSERT_TRUE(txn.ok());
    for (int i = 0; i < 1000; ++i) {
      ASSERT_TRUE((*txn)->Put(ColumnFamily::kMeta, "k" + std::to_string(i), std::string(100, 'v')).ok());
    }
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  ASSERT_TRUE(s->Checkpoint().ok());
  auto scan = [&](bool fill_cache) {
    std::vector<std::pair<std::string, std::string>> rows;
    auto view = s->BeginRead();
    EXPECT_TRUE(view.ok());
    IterOptions opts;
    opts.fill_cache = fill_cache;
    auto it = (*view)->NewIterator(ColumnFamily::kMeta, std::move(opts));
    EXPECT_TRUE(it.ok());
    for (Status st = (*it)->SeekToFirst(); st.ok() && (*it)->Valid(); st = (*it)->Next())
      rows.emplace_back(std::string((*it)->key()), std::string((*it)->value()));
    return rows;
  };
  auto before = s->Stats();
  ASSERT_TRUE(before.ok());
  const auto uncached = scan(false);
  EXPECT_EQ(uncached.size(), 1000u);
  auto after = s->Stats();
  ASSERT_TRUE(after.ok());
  EXPECT_EQ(after->block_cache_entries, before->block_cache_entries);
  EXPECT_EQ(after->block_cache_bytes, before->block_cache_bytes);

  EXPECT_EQ(scan(true), uncached);
  auto cached = s->Stats();
  ASSERT_TRUE(cached.ok());
  EXPECT_GT(cached->block_cache_entries, before->block_cache_entries);
}

// B10: the queued-memtable byte budget is raised to two memtables when set smaller, so a lone sealed
// memtable never blocks writers by itself; 0 leaves only the count limit. The stats carry the queue's bytes.
TEST(TidesStorageMaint, QueueBudgetIsClamped) {
  {
    TempDir dir;
    StorageOptions o = OptionsFor(dir);
    o.memtable_size = 8ULL << 20;
    o.memtable_queue_max_bytes = 4ULL << 20;
    auto s = MustOpen(o);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->memtable_queue_max_bytes(), 16ULL << 20);
    auto st = s->Stats();
    ASSERT_TRUE(st.ok());
    EXPECT_EQ(st->immutable_memtable_bytes, 0u);
    EXPECT_EQ(st->writes_blocked, 0u);
  }
  {
    TempDir dir;
    StorageOptions o = OptionsFor(dir);
    o.memtable_size = 8ULL << 20;
    o.memtable_queue_max_bytes = 0;
    auto s = MustOpen(o);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->memtable_queue_max_bytes(), 0u);
  }
  {
    TempDir dir;
    StorageOptions o = OptionsFor(dir);
    o.memtable_size = 8ULL << 20;
    o.memtable_queue_max_bytes = 64ULL << 20;  // already above two memtables: kept as given
    auto s = MustOpen(o);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->memtable_queue_max_bytes(), 64ULL << 20);
  }
}

// The block size and the compression start level reach every family, on creation and on reopen, and a
// reopen with the settings the families already carry issues no config update.
TEST(TidesStorageMaint, BlockSizeAndCompressionStartLevelApplyAndSkipUnchangedUpdates) {
  TempDir dir;
  StorageOptions o = OptionsFor(dir);
  o.btree_block_size = 16384;
  o.compression_start_level = 2;
  {
    auto s = MustOpen(o);
    ASSERT_NE(s, nullptr);
    auto st = s->Stats();
    ASSERT_TRUE(st.ok());
    for (const CfStats& cf : st->cf) {
      EXPECT_EQ(cf.btree_block_size, 16384u);
      EXPECT_EQ(cf.compression_start_level, 2);
    }
  }
  {  // the same values: nothing to update
    auto s = MustOpen(o);
    ASSERT_NE(s, nullptr);
    auto st = s->Stats();
    ASSERT_TRUE(st.ok());
    EXPECT_EQ(st->family_config_updates, 0u);
    for (const CfStats& cf : st->cf) EXPECT_EQ(cf.btree_block_size, 16384u);
  }
  o.btree_block_size = 32768;
  {  // a changed value reaches the existing families
    auto s = MustOpen(o);
    ASSERT_NE(s, nullptr);
    auto st = s->Stats();
    ASSERT_TRUE(st.ok());
    EXPECT_EQ(st->family_config_updates, static_cast<uint64_t>(kColumnFamilyCount));
    for (const CfStats& cf : st->cf) {
      EXPECT_EQ(cf.btree_block_size, 32768u);
      EXPECT_EQ(cf.compression_start_level, 2);
    }
  }
  o.compression_start_level = 0;
  {
    auto s = MustOpen(o);
    ASSERT_NE(s, nullptr);
    auto st = s->Stats();
    ASSERT_TRUE(st.ok());
    for (const CfStats& cf : st->cf) EXPECT_EQ(cf.compression_start_level, 0);
  }
  TempDir fresh;  // the defaults are today's behaviour
  auto s = MustOpen(OptionsFor(fresh));
  ASSERT_NE(s, nullptr);
  auto st = s->Stats();
  ASSERT_TRUE(st.ok());
  for (const CfStats& cf : st->cf) {
    EXPECT_EQ(cf.btree_block_size, 4096u);
    EXPECT_EQ(cf.compression_start_level, 0);
  }
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
