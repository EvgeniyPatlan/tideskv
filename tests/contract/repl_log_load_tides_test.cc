// The exact snapshot cut over the real engine (M6b D2, spike s_snapcut): ReplLog::ActivateAndSnapshot takes
// the TidesDB snapshot and the watermark S together under the commit-section mutex, so while writers keep
// committing (a small memtable: rotations and flushes underneath the open snapshot) the view holds every
// batch 1..S, each complete, and nothing after S. Also a replica's load transactions (M6b D5) on TidesDB.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "encoding/replog_codec.h"
#include "exec/repl_log.h"
#include "storage/tides_storage.h"
#include "support/fake_clock.h"
#include "support/paced_writers.h"
#include "support/snapshot_cut.h"
#include "support/temp_dir.h"

namespace tideskv::test {
namespace {

using namespace std::chrono_literals;
using encoding::ReplOp;
using encoding::ReplOpType;

constexpr int kWriters = 4;
constexpr uint64_t kRounds = 50;
constexpr uint64_t kStep = 400;   // commits granted per round: bounds the store, hence each round's scan
constexpr int kProbes = 100;      // at most, per round, while the first half of its commits lands
constexpr int kKeysPerBatch = 4;  // keyspace puts per batch, all in one txn
constexpr size_t kMemtableBytes = 256 << 10;  // as in the spike: rotations and flushes during the test

StorageOptions Opts(const TempDir& dir, size_t memtable_bytes = 0) {
  StorageOptions o;
  o.path = (dir.path() / "db").string();
  o.sync_mode = SyncMode::kNone;
  o.memtable_size = memtable_bytes;
  return o;
}

// One batch of kKeysPerBatch distinct meta keys with 200-byte values, committed through the log.
Status CommitBatch(ReplLog& log, Storage& s, const std::string& prefix) {
  auto txn = s.BeginWrite();
  if (!txn.ok()) return txn.status();
  const std::string value(200, 'v');
  std::vector<ReplOp> ops;
  for (int j = 0; j < kKeysPerBatch; ++j) {
    ops.push_back({ReplOpType::kPut, ColumnFamily::kMeta, prefix + ":" + std::to_string(j), value});
    if (Status st = (*txn)->Put(ColumnFamily::kMeta, ops.back().key, value); !st.ok()) return st;
  }
  return log.Commit(**txn, ops);
}

// Counts a CF's keys; for the replog also checks that the keys are exactly the seqs 1..n.
uint64_t CountCf(ReadView& v, ColumnFamily cf) {
  auto it = v.NewIterator(cf);
  EXPECT_TRUE(it.ok());
  if (!it.ok()) return 0;
  uint64_t n = 0;
  Status st = (*it)->SeekToFirst();
  for (; st.ok() && (*it)->Valid(); st = (*it)->Next()) {
    ++n;
    uint64_t seq = 0;
    if (cf == ColumnFamily::kReplog) {
      EXPECT_TRUE(encoding::DecodeReplogKey((*it)->key(), &seq));
      EXPECT_EQ(seq, n);
    }
  }
  EXPECT_TRUE(st.ok() || st.IsNotFound()) << st.ToString();
  return n;
}

// Probes cuts until last() reaches `until` (at most kProbes of them), counting good and bad boundaries,
// then waits for `until`. Fatal failures: call it under ASSERT_NO_FATAL_FAILURE.
void ProbeUntil(ReplLog& log, uint64_t until, int* good, int* bad) {
  for (int i = 0; i < kProbes && log.last() < until; ++i) {
    auto probe = log.ActivateAndSnapshot();
    ASSERT_TRUE(probe.ok()) << probe.status().ToString();
    ++(CutBoundaryHolds(*probe) ? *good : *bad);
  }
  ASSERT_GE(log.WaitBeyond(until - 1, 30s), until) << "writers stalled before " << until;
}

TEST(ReplLogSnapshotCutTides, ActivateAndSnapshotIsAnExactCutUnderConcurrentCommits) {
  TempDir dir("tideskv-snapcut");
  auto s = TidesStorage::Open(Opts(dir, kMemtableBytes));
  ASSERT_TRUE(s.ok()) << s.status().ToString();
  FakeClock clock;
  ReplLog log(**s, clock);
  ASSERT_TRUE(log.Recover().ok());

  PacedWriters writers(kWriters, [&](int w, uint64_t i) {
    return CommitBatch(log, **s, std::to_string(w) + ":" + std::to_string(i));
  });
  int raced = 0;  // rounds in which commits landed while the cut was open
  int probes = 0;
  int bad_probes = 0;
  for (uint64_t round = 0; round < kRounds; ++round) {
    const uint64_t half = round * kStep + kStep / 2;
    writers.Grant(half);  // first half: cheap boundary probes while these commits land
    ASSERT_NO_FATAL_FAILURE(ProbeUntil(log, half, &probes, &bad_probes));
    writers.Grant((round + 1) * kStep);  // second half: one full-scan cut amid these commits
    ASSERT_GT(log.WaitBeyond(half + kStep / 4, 30s), half + kStep / 4)
        << "writers stalled in round " << round;
    auto snap = log.ActivateAndSnapshot();
    ASSERT_TRUE(snap.ok()) << snap.status().ToString();
    EXPECT_EQ(CountCf(*snap->view, ColumnFamily::kReplog), snap->seq) << "round " << round;
    EXPECT_EQ(CountCf(*snap->view, ColumnFamily::kMeta), snap->seq * kKeysPerBatch) << "round " << round;
    if (log.last() > snap->seq) ++raced;
  }
  EXPECT_EQ(writers.failures(), 0);
  EXPECT_GT(raced, 0);
  EXPECT_EQ(bad_probes, 0) << "of " << probes + bad_probes << " boundary probes";
  RecordProperty("probes", std::to_string(probes));
  RecordProperty("raced_rounds", std::to_string(raced));
  RecordProperty("final_seq", std::to_string(log.last()));
}

// Start's whole-CF range delete, End's baseline and the marker survive a restart of the real engine.
TEST(ReplLogLoadTides, LoadTransactionsAndTheMarkerSurviveARestart) {
  TempDir dir("tideskv-load");
  FakeClock clock;
  const std::string record =
      encoding::EncodeReplogBatch({{ReplOpType::kPut, ColumnFamily::kMeta, "a", "1"}}, 5, 1000);
  {
    auto s = TidesStorage::Open(Opts(dir));
    ASSERT_TRUE(s.ok()) << s.status().ToString();
    ReplLog log(**s, clock);
    ASSERT_TRUE(log.Recover().ok());
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(CommitBatch(log, **s, "k" + std::to_string(i)).ok());
    log.FenceLocalWrites(true);
    auto txn = (*s)->BeginWrite();
    ASSERT_TRUE(txn.ok());
    ASSERT_TRUE(log.CommitLoadStart(**txn).ok());
  }
  {
    auto s = TidesStorage::Open(Opts(dir));  // killed mid-load: the marker is reported, the replog is gone
    ASSERT_TRUE(s.ok()) << s.status().ToString();
    ReplLog log(**s, clock);
    ASSERT_TRUE(log.Recover().ok());
    EXPECT_TRUE(log.load_incomplete());
    EXPECT_EQ(log.last(), 0u);
    auto view = (*s)->BeginRead();
    ASSERT_TRUE(view.ok());
    EXPECT_EQ(CountCf(**view, ColumnFamily::kReplog), 0u);
    log.FenceLocalWrites(true);
    auto txn = (*s)->BeginWrite();
    ASSERT_TRUE(txn.ok());
    ASSERT_TRUE(log.CommitLoadEnd(**txn, 42, record).ok());
  }
  auto s = TidesStorage::Open(Opts(dir));
  ASSERT_TRUE(s.ok()) << s.status().ToString();
  ReplLog log(**s, clock);
  ASSERT_TRUE(log.Recover().ok());
  EXPECT_FALSE(log.load_incomplete());
  EXPECT_EQ(log.last(), 42u);
  EXPECT_EQ(log.first_retained(), 42u);
  EXPECT_EQ(log.cum_bytes(), 1000 + record.size());
}

}  // namespace
}  // namespace tideskv::test
