// ReplLog's exact snapshot cut and a replica's full-sync load transactions (M6b D2, D5).
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "encoding/replog_codec.h"
#include "exec/repl_log.h"
#include "repl/repl_state.h"
#include "storage/memory_storage.h"
#include "support/fake_clock.h"
#include "support/paced_writers.h"
#include "support/snapshot_cut.h"

namespace tideskv {
namespace {

using encoding::ReplOp;
using encoding::ReplOpType;
using test::CutBoundaryHolds;
using test::FakeClock;

Status CommitKey(ReplLog& log, Storage& s, const std::string& key) {
  auto txn = s.BeginWrite();
  if (!txn.ok()) return txn.status();
  const std::vector<ReplOp> ops = {{ReplOpType::kPut, ColumnFamily::kMeta, key, "v"}};
  if (Status st = (*txn)->Put(ColumnFamily::kMeta, key, "v"); !st.ok()) return st;
  return log.Commit(**txn, ops);
}

size_t CountCf(ReadView& v, ColumnFamily cf) {
  auto it = v.NewIterator(cf);
  EXPECT_TRUE(it.ok());
  if (!it.ok()) return 0;
  size_t n = 0;
  for (Status st = (*it)->SeekToFirst(); st.ok() && (*it)->Valid(); st = (*it)->Next()) ++n;
  return n;
}

struct LoadLog : ::testing::Test {
  InMemoryStorage s;
  FakeClock clock{1000};
  ReplLog log{s, clock};
  void SetUp() override { ASSERT_TRUE(log.Recover().ok()); }
  void Start() {
    log.FenceLocalWrites(true);
    auto txn = s.BeginWrite();
    ASSERT_TRUE(txn.ok());
    ASSERT_TRUE(log.CommitLoadStart(**txn).ok());
  }
};

TEST(ReplLogLoadNames, MarkerAndBaselineKeysAreTheSharedNames) {
  EXPECT_EQ(kSysReplLoadIncomplete, "repl_load_incomplete");
  EXPECT_EQ(repl::kSysReplBaselineUnknown, "repl_baseline_unknown");  // repl_log.cc uses this literal
}

TEST_F(LoadLog, SnapshotAtLastIsAnExactCutUnderConcurrentCommits) {
  constexpr uint64_t kStep = 40;  // commits granted per round (InMemoryStorage copies its tables per commit)
  test::PacedWriters writers(
      4, [&](int w, uint64_t i) { return CommitKey(log, s, std::to_string(w) + ":" + std::to_string(i)); });
  int bad_probes = 0;
  for (uint64_t round = 0; round < 50; ++round) {
    writers.Grant((round + 1) * kStep);
    const uint64_t mid = round * kStep + kStep / 2;  // cut while commits are in flight
    ASSERT_GT(log.WaitBeyond(mid, std::chrono::seconds(30)), mid) << "writers stalled in round " << round;
    for (int i = 0; i < 100 && log.last() < (round + 1) * kStep; ++i) {
      auto probe = log.SnapshotAtLast();
      ASSERT_TRUE(probe.ok());
      if (!CutBoundaryHolds(*probe)) ++bad_probes;
    }
    auto snap = log.SnapshotAtLast();
    ASSERT_TRUE(snap.ok());
    EXPECT_EQ(CountCf(*snap->view, ColumnFamily::kReplog), snap->seq);  // replog 1..seq and nothing more
    EXPECT_EQ(CountCf(*snap->view, ColumnFamily::kMeta), snap->seq);    // one key per batch
  }
  EXPECT_EQ(writers.failures(), 0);
  EXPECT_EQ(bad_probes, 0);
}

TEST_F(LoadLog, StartRequiresTheFenceThenEmptiesTheLogAndSetsTheMarker) {
  for (int i = 0; i < 5; ++i) ASSERT_TRUE(CommitKey(log, s, "k" + std::to_string(i)).ok());
  {
    auto txn = s.BeginWrite();
    ASSERT_TRUE(txn.ok());
    ASSERT_TRUE((*txn)->Put(ColumnFamily::kSys, "repl_baseline_unknown", "1").ok());
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  auto txn = s.BeginWrite();
  ASSERT_TRUE(txn.ok());
  EXPECT_EQ(log.CommitLoadStart(**txn).code(), StatusCode::kInternal);  // not fenced
  Start();
  EXPECT_EQ(log.last(), 0u);
  EXPECT_EQ(log.first_retained(), 1u);
  EXPECT_EQ(log.cum_bytes(), 0u);
  EXPECT_TRUE(log.load_incomplete());
  auto v = s.BeginRead();
  ASSERT_TRUE(v.ok());
  EXPECT_EQ(CountCf(**v, ColumnFamily::kReplog), 0u);
  EXPECT_TRUE((*v)->Get(ColumnFamily::kSys, "repl_baseline_unknown").status().IsNotFound());
  EXPECT_TRUE((*v)->Get(ColumnFamily::kSys, "repl_first").status().IsNotFound());
  ReplLog again(s, clock);
  ASSERT_TRUE(again.Recover().ok());
  EXPECT_TRUE(again.load_incomplete());
  EXPECT_EQ(again.last(), 0u);
}

TEST_F(LoadLog, EndInstallsTheMastersBaselineAndTheStreamContinues) {
  Start();
  const std::vector<ReplOp> ops = {{ReplOpType::kPut, ColumnFamily::kMeta, "a", "1"}};
  const std::string record = encoding::EncodeReplogBatch(ops, 5, 1000);  // the master's replog[42]
  auto txn = s.BeginWrite();
  ASSERT_TRUE(txn.ok());
  ASSERT_TRUE(log.CommitLoadEnd(**txn, 42, record).ok());
  EXPECT_EQ(log.last(), 42u);
  EXPECT_EQ(log.first_retained(), 42u);
  EXPECT_EQ(log.cum_bytes(), 1000 + record.size());
  EXPECT_FALSE(log.load_incomplete());
  ReplLog again(s, clock);
  ASSERT_TRUE(again.Recover().ok());
  EXPECT_EQ(again.last(), 42u);
  EXPECT_EQ(again.first_retained(), 42u);
  EXPECT_EQ(again.cum_bytes(), 1000 + record.size());
  EXPECT_FALSE(again.load_incomplete());
  const std::string next = encoding::EncodeReplogBatch(ops, 6, 1000 + record.size());  // CF2 continuity
  txn = s.BeginWrite();
  ASSERT_TRUE(txn.ok());
  EXPECT_TRUE(log.CommitReplicated(**txn, 43, next).ok());
}

TEST_F(LoadLog, EndAtSeqZeroLeavesAnEmptyLog) {
  Start();
  auto txn = s.BeginWrite();
  ASSERT_TRUE(txn.ok());
  EXPECT_EQ(log.CommitLoadEnd(**txn, 0, "x").code(), StatusCode::kCorruption);
  txn = s.BeginWrite();
  ASSERT_TRUE(txn.ok());
  ASSERT_TRUE(log.CommitLoadEnd(**txn, 0, "").ok());
  EXPECT_EQ(log.last(), 0u);
  EXPECT_EQ(log.first_retained(), 1u);
  EXPECT_FALSE(log.load_incomplete());
}

TEST_F(LoadLog, EndRejectsACorruptRecordAndCommitsNothing) {
  Start();
  std::string record = encoding::EncodeReplogBatch({{ReplOpType::kPut, ColumnFamily::kMeta, "a", "1"}}, 5, 0);
  record[record.size() / 2] ^= 1;
  auto txn = s.BeginWrite();
  ASSERT_TRUE(txn.ok());
  EXPECT_EQ(log.CommitLoadEnd(**txn, 7, record).code(), StatusCode::kCorruption);
  EXPECT_TRUE(log.load_incomplete());
  EXPECT_EQ(log.last(), 0u);
}

// G1 (final review #1): what a replica whose abort had failed could leave behind: the marker, plus a log
// that batches applied onto the partial keyspace extended (replog[1..3], repl_first 2) and the F5 flag.
void WriteMarkedLogWithBatches(Storage& s) {
  auto txn = s.BeginWrite();
  ASSERT_TRUE(txn.ok());
  uint64_t cum = 0;
  for (uint64_t seq = 1; seq <= 3; ++seq) {
    const std::string record =
        encoding::EncodeReplogBatch({{ReplOpType::kPut, ColumnFamily::kMeta, "k", "v"}}, 5, cum);
    cum += record.size();
    ASSERT_TRUE((*txn)->Put(ColumnFamily::kReplog, encoding::ReplogKey(seq), record).ok());
  }
  ASSERT_TRUE((*txn)->Put(ColumnFamily::kSys, "repl_first", std::string("\0\0\0\0\0\0\0\x02", 8)).ok());
  ASSERT_TRUE((*txn)->Put(ColumnFamily::kSys, "repl_baseline_unknown", "1").ok());
  ASSERT_TRUE((*txn)->Put(ColumnFamily::kSys, kSysReplLoadIncomplete, "1").ok());
  ASSERT_TRUE((*txn)->Commit().ok());
}

// G1: the abort's postcondition does not depend on the call sequence: whatever the log holds, an abort
// leaves the same seq-0 node as the start txn (empty replog, no repl_first, baseline 0/0/1), in memory
// and after a restart.
TEST_F(LoadLog, AbortEmptiesTheLogAndResetsTheBaseline) {
  WriteMarkedLogWithBatches(s);
  ReplLog marked(s, clock);
  ASSERT_TRUE(marked.Recover().ok());
  ASSERT_TRUE(marked.load_incomplete());
  ASSERT_EQ(marked.last(), 3u);
  ASSERT_EQ(marked.first_retained(), 2u);
  auto txn = s.BeginWrite();
  ASSERT_TRUE(txn.ok());
  ASSERT_TRUE(marked.CommitLoadAbort(**txn).ok());
  EXPECT_FALSE(marked.load_incomplete());
  EXPECT_EQ(marked.last(), 0u);
  EXPECT_EQ(marked.first_retained(), 1u);
  EXPECT_EQ(marked.cum_bytes(), 0u);
  auto v = s.BeginRead();
  ASSERT_TRUE(v.ok());
  EXPECT_EQ(CountCf(**v, ColumnFamily::kReplog), 0u);
  EXPECT_TRUE((*v)->Get(ColumnFamily::kSys, "repl_first").status().IsNotFound());
  EXPECT_TRUE((*v)->Get(ColumnFamily::kSys, "repl_baseline_unknown").status().IsNotFound());
  ReplLog again(s, clock);
  ASSERT_TRUE(again.Recover().ok());
  EXPECT_FALSE(again.load_incomplete());
  EXPECT_EQ(again.last(), 0u);
  EXPECT_EQ(again.first_retained(), 1u);
  EXPECT_EQ(again.cum_bytes(), 0u);
}

// G1: no batch extends a log whose keyspace is a partial load (the marker is the durable "partial").
TEST_F(LoadLog, CommitReplicatedIsRefusedDuringALoad) {
  Start();
  const std::string record =
      encoding::EncodeReplogBatch({{ReplOpType::kPut, ColumnFamily::kMeta, "a", "1"}}, 5, 0);
  auto txn = s.BeginWrite();
  ASSERT_TRUE(txn.ok());
  EXPECT_EQ(log.CommitReplicated(**txn, 1, record).code(), StatusCode::kInternal);
  EXPECT_EQ(log.last(), 0u);
}

TEST_F(LoadLog, AbortClearsTheMarker) {
  Start();
  auto txn = s.BeginWrite();
  ASSERT_TRUE(txn.ok());
  ASSERT_TRUE(log.CommitLoadAbort(**txn).ok());
  EXPECT_FALSE(log.load_incomplete());
  ReplLog again(s, clock);
  ASSERT_TRUE(again.Recover().ok());
  EXPECT_FALSE(again.load_incomplete());
}

}  // namespace
}  // namespace tideskv
