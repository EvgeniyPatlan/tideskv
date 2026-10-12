// The background compaction the scheduler issues must physically drop range-deleted data:
// until it does, every unbounded engine iterator steps over the dead keys one by one.
#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "encoding/key_codec.h"
#include "server/compaction_scheduler.h"
#include "storage/tides_storage.h"
#include "support/temp_dir.h"

namespace tideskv::test {
namespace {

constexpr int kDeadKeys = 20000;
constexpr int kLiveKeysPerDb = 50;

std::unique_ptr<TidesStorage> OpenStorage(const TempDir& dir) {
  StorageOptions o;
  o.path = (dir.path() / "db").string();
  o.sync_mode = SyncMode::kNone;
  o.compression = Compression::kNone;
  o.memtable_size = 64 * 1024;  // many flushes and engine compactions, as under real load
  auto s = TidesStorage::Open(o);
  EXPECT_TRUE(s.ok()) << s.status().ToString();
  return s.ok() ? std::move(s).value() : nullptr;
}

std::string Key(uint8_t db, int i) { return encoding::DbPrefix(db) + "k" + std::to_string(i); }

void PutKeys(TidesStorage& s, ColumnFamily cf, uint8_t db, int n) {
  auto txn = s.BeginWrite();
  ASSERT_TRUE(txn.ok());
  for (int i = 0; i < n; ++i) ASSERT_TRUE((*txn)->Put(cf, Key(db, i), "v").ok());
  ASSERT_TRUE((*txn)->Commit().ok());
  ASSERT_TRUE(s.Checkpoint().ok());
}

uint64_t SstableKeys(TidesStorage& s, ColumnFamily cf) {
  auto stats = s.Stats();
  EXPECT_TRUE(stats.ok()) << stats.status().ToString();
  return stats.ok() ? stats->cf[CfIndex(cf)].total_keys : 0;
}

uint64_t CountKeys(TidesStorage& s, ColumnFamily cf, uint8_t db) {
  auto view = s.BeginRead();
  EXPECT_TRUE(view.ok());
  auto it =
      (*view)->NewIterator(cf, {encoding::DbPrefix(db), encoding::PrefixUpperBound(encoding::DbPrefix(db))});
  EXPECT_TRUE(it.ok());
  uint64_t n = 0;
  Status st = (*it)->SeekToFirst();
  for (; st.ok() && (*it)->Valid(); st = (*it)->Next()) ++n;
  EXPECT_TRUE(st.ok()) << st.ToString();
  return n;
}

// Runs the scheduler's queue to the end. The engine's own compactions can hold the CF (Busy) for a
// while after the bulk load; the job is retried on the next pass, as the scheduler's ticks would.
void Drain(CompactionScheduler& sched) {
  for (int i = 0; i < 1000 && sched.pending() > 0; ++i) {
    if (sched.RunPending() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_EQ(sched.pending(), 0u);
  ASSERT_EQ(sched.failed(), 0u);
}

class CompactionReclaim : public ::testing::TestWithParam<ColumnFamily> {};

TEST_P(CompactionReclaim, FlushDbCompactionPhysicallyDropsThatDbOnly) {
  const ColumnFamily cf = GetParam();
  TempDir dir;
  auto s = OpenStorage(dir);
  ASSERT_NE(s, nullptr);
  PutKeys(*s, cf, 0, kLiveKeysPerDb);
  PutKeys(*s, cf, 2, kDeadKeys);
  PutKeys(*s, cf, 3, kLiveKeysPerDb);
  {
    auto txn = s->BeginWrite();
    ASSERT_TRUE((*txn)->DeletePrefix(cf, encoding::DbPrefix(2)).ok());
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  ASSERT_TRUE(s->Checkpoint().ok());  // the tombstone alone in an sstable, as after a real FLUSHDB
  ASSERT_GE(SstableKeys(*s, cf), uint64_t{kDeadKeys});  // the dead keys are still on disk
  CompactionScheduler::Options opts;
  opts.max_attempts = 1000;
  CompactionScheduler sched(*s, opts);
  sched.OnCommitted({{cf, encoding::DbPrefix(2), {}, true}});  // what the Engine reports for FLUSHDB 2
  Drain(sched);

  EXPECT_LE(SstableKeys(*s, cf), uint64_t{2 * kLiveKeysPerDb});  // only the live keys are left
  EXPECT_EQ(CountKeys(*s, cf, 0), uint64_t{kLiveKeysPerDb});
  EXPECT_EQ(CountKeys(*s, cf, 2), 0u);
  EXPECT_EQ(CountKeys(*s, cf, 3), uint64_t{kLiveKeysPerDb});
}

INSTANTIATE_TEST_SUITE_P(KeyspaceCfs, CompactionReclaim,
                         ::testing::Values(ColumnFamily::kMeta, ColumnFamily::kSubkey, ColumnFamily::kZScore,
                                           ColumnFamily::kExpire),
                         [](const auto& p) { return std::string(ColumnFamilyName(p.param)); });

}  // namespace
}  // namespace tideskv::test
