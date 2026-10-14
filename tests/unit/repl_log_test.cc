#include "exec/repl_log.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "common/coding.h"
#include "encoding/replog_codec.h"
#include "storage/memory_storage.h"
#include "support/fake_clock.h"
#include "support/faulty_storage.h"
#include "types/errors.h"

namespace tideskv {
namespace {

using encoding::ReplOp;
using encoding::ReplOpType;
using test::FakeClock;
using test::FaultyStorage;

std::vector<ReplOp> MetaPut(const std::string& key, const std::string& value = "v") {
  return {{ReplOpType::kPut, ColumnFamily::kMeta, key, value}};
}

// Writes `ops` (puts only) on a fresh txn and commits it through `log`.
Status CommitPuts(Storage& s, ReplLog& log, const std::vector<ReplOp>& ops) {
  auto txn = s.BeginWrite();
  if (!txn.ok()) return txn.status();
  for (const ReplOp& op : ops) {
    if (Status st = (*txn)->Put(op.cf, op.key, op.value); !st.ok()) return st;
  }
  return log.Commit(**txn, ops);
}

Result<std::string> ReplogAt(Storage& s, uint64_t seq) {
  auto view = s.BeginRead();
  if (!view.ok()) return view.status();
  return (*view)->Get(ColumnFamily::kReplog, encoding::ReplogKey(seq));
}

TEST(ReplLogTest, CommitAssignsConsecutiveSeqsAndWritesTheBatch) {
  InMemoryStorage s;
  FakeClock clock(1000);
  ReplLog log(s, clock);
  ASSERT_TRUE(log.Recover().ok());
  EXPECT_EQ(log.last(), 0u);
  EXPECT_EQ(log.first_retained(), 1u);
  for (int i = 1; i <= 3; ++i) {
    auto txn = s.BeginWrite();
    std::vector<ReplOp> ops = MetaPut("k" + std::to_string(i));
    ASSERT_TRUE((*txn)->Put(ColumnFamily::kMeta, ops[0].key, "v").ok());
    ASSERT_TRUE(log.Commit(**txn, ops).ok());
    EXPECT_EQ(log.last(), static_cast<uint64_t>(i));
  }
  auto raw = ReplogAt(s, 2);
  ASSERT_TRUE(raw.ok());
  auto batch = encoding::DecodeReplogBatch(*raw);
  ASSERT_TRUE(batch.ok());
  ASSERT_EQ(batch->ops.size(), 1u);
  EXPECT_EQ(batch->ops[0].key, "k2");
  EXPECT_EQ(batch->timestamp_ms, 1000);
  auto view = s.BeginRead();
  EXPECT_EQ(*(*view)->Get(ColumnFamily::kMeta, "k3"), "v");  // the caller's writes committed too
}

TEST(ReplLogTest, EmptyOpsCommitWithoutASeq) {
  InMemoryStorage s;
  FakeClock clock;
  ReplLog log(s, clock);
  ASSERT_TRUE(log.Recover().ok());
  auto txn = s.BeginWrite();
  ASSERT_TRUE(log.Commit(**txn, {}).ok());
  EXPECT_EQ(log.last(), 0u);
  EXPECT_EQ(log.cum_bytes(), 0u);
  EXPECT_TRUE(ReplogAt(s, 1).status().IsNotFound());
}

TEST(ReplLogTest, FailedCommitConsumesNoSeq) {
  FaultyStorage s;
  FakeClock clock;
  ReplLog log(s, clock);
  ASSERT_TRUE(log.Recover().ok());
  ASSERT_TRUE(CommitPuts(s, log, MetaPut("a")).ok());
  const uint64_t cum = log.cum_bytes();
  s.fail_commit_in = 1;
  EXPECT_FALSE(CommitPuts(s, log, MetaPut("b")).ok());
  EXPECT_EQ(log.last(), 1u);
  EXPECT_EQ(log.cum_bytes(), cum);
  ASSERT_TRUE(CommitPuts(s, log, MetaPut("c")).ok());
  EXPECT_EQ(log.last(), 2u);
  auto raw = ReplogAt(s, 2);
  ASSERT_TRUE(raw.ok());
  auto batch = encoding::DecodeReplogBatch(*raw);
  ASSERT_TRUE(batch.ok());
  EXPECT_EQ(batch->ops, MetaPut("c"));
  EXPECT_TRUE(ReplogAt(s, 3).status().IsNotFound());
}

TEST(ReplLogTest, CumBytesAccumulate) {
  InMemoryStorage s;
  FakeClock clock;
  ReplLog log(s, clock);
  ASSERT_TRUE(log.Recover().ok());
  uint64_t sum = 0;
  for (int i = 1; i <= 3; ++i) {
    ASSERT_TRUE(CommitPuts(s, log, MetaPut("k" + std::to_string(i), std::string(i * 10, 'x'))).ok());
    sum += ReplogAt(s, i)->size();
    EXPECT_EQ(log.cum_bytes(), sum);
  }
  int64_t ts = 0;
  uint64_t stored_cum = 0;
  ASSERT_TRUE(encoding::PeekReplogHeader(*ReplogAt(s, 3), &ts, &stored_cum));
  EXPECT_EQ(stored_cum, sum);
}

TEST(ReplLogTest, RecoverReadsLastSeqAndCum) {
  InMemoryStorage s;
  FakeClock clock;
  uint64_t cum = 0;
  {
    ReplLog log(s, clock);
    ASSERT_TRUE(log.Recover().ok());
    for (int i = 1; i <= 5; ++i) ASSERT_TRUE(CommitPuts(s, log, MetaPut("k" + std::to_string(i))).ok());
    cum = log.cum_bytes();
  }
  ReplLog again(s, clock);
  ASSERT_TRUE(again.Recover().ok());
  EXPECT_EQ(again.last(), 5u);
  EXPECT_EQ(again.cum_bytes(), cum);
  EXPECT_EQ(again.first_retained(), 1u);
  ASSERT_TRUE(CommitPuts(s, again, MetaPut("k6")).ok());
  EXPECT_EQ(again.last(), 6u);
}

TEST(ReplLogTest, RecoverTakesFirstRetainedFromSysOrTheOldestEntry) {
  InMemoryStorage s;
  FakeClock clock;
  {
    ReplLog log(s, clock);
    ASSERT_TRUE(log.Recover().ok());
    for (int i = 1; i <= 4; ++i) ASSERT_TRUE(CommitPuts(s, log, MetaPut("k" + std::to_string(i))).ok());
  }
  {  // no sys/repl_first and entries 1..2 gone: the oldest entry is the first retained
    auto txn = s.BeginWrite();
    ASSERT_TRUE((*txn)->Delete(ColumnFamily::kReplog, encoding::ReplogKey(1)).ok());
    ASSERT_TRUE((*txn)->Delete(ColumnFamily::kReplog, encoding::ReplogKey(2)).ok());
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  ReplLog oldest(s, clock);
  ASSERT_TRUE(oldest.Recover().ok());
  EXPECT_EQ(oldest.first_retained(), 3u);
  EXPECT_EQ(oldest.last(), 4u);
  {
    std::string first;
    coding::PutFixed64BE(&first, 4);
    auto txn = s.BeginWrite();
    ASSERT_TRUE((*txn)->Put(ColumnFamily::kSys, "repl_first", first).ok());
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  ReplLog stored(s, clock);
  ASSERT_TRUE(stored.Recover().ok());
  EXPECT_EQ(stored.first_retained(), 4u);
  stored.SetFirstRetained(5);
  EXPECT_EQ(stored.first_retained(), 5u);
}

TEST(ReplLogTest, RecoverRejectsAMalformedReplogKeyOrFirst) {
  {
    InMemoryStorage s;
    FakeClock clock;
    auto txn = s.BeginWrite();
    ASSERT_TRUE((*txn)->Put(ColumnFamily::kSys, "repl_first", "short").ok());
    ASSERT_TRUE((*txn)->Commit().ok());
    ReplLog log(s, clock);
    EXPECT_EQ(log.Recover().code(), StatusCode::kCorruption);
  }
  {
    InMemoryStorage s;
    FakeClock clock;
    auto txn = s.BeginWrite();
    ASSERT_TRUE((*txn)->Put(ColumnFamily::kReplog, "bad", "x").ok());
    ASSERT_TRUE((*txn)->Commit().ok());
    ReplLog log(s, clock);
    EXPECT_EQ(log.Recover().code(), StatusCode::kCorruption);
  }
}

TEST(ReplLogTest, FencedCommitIsRefusedWithReadOnly) {
  InMemoryStorage s;
  FakeClock clock;
  ReplLog log(s, clock);
  ASSERT_TRUE(log.Recover().ok());
  log.FenceLocalWrites(true);
  EXPECT_TRUE(log.fenced());
  const Status st = CommitPuts(s, log, MetaPut("a"));
  EXPECT_EQ(st.code(), StatusCode::kUserError);
  EXPECT_EQ(st.message(), kErrReadOnlyReplica);
  EXPECT_EQ(log.last(), 0u);
  auto txn = s.BeginWrite();
  EXPECT_TRUE(log.Commit(**txn, {}).ok());  // wrote nothing: nothing to refuse
  auto rep = s.BeginWrite();
  ASSERT_TRUE((*rep)->Put(ColumnFamily::kMeta, "a", "v").ok());
  EXPECT_TRUE(log.CommitReplicated(**rep, 1, encoding::EncodeReplogBatch(MetaPut("a"), 5, 0)).ok());
  EXPECT_EQ(log.last(), 1u);
  log.FenceLocalWrites(false);
  EXPECT_TRUE(CommitPuts(s, log, MetaPut("b")).ok());
  EXPECT_EQ(log.last(), 2u);
}

TEST(ReplLogTest, CommitReplicatedRequiresTheNextSeqAndStoresPayloadVerbatim) {
  InMemoryStorage s;
  FakeClock clock;
  ReplLog log(s, clock);
  ASSERT_TRUE(log.Recover().ok());
  const std::string payload = encoding::EncodeReplogBatch(MetaPut("a"), 77, 0);
  {
    auto txn = s.BeginWrite();
    EXPECT_EQ(log.CommitReplicated(**txn, 2, payload).code(), StatusCode::kInternal);
    EXPECT_EQ(log.last(), 0u);
  }
  auto txn = s.BeginWrite();
  ASSERT_TRUE((*txn)->Put(ColumnFamily::kMeta, "a", "v").ok());
  ASSERT_TRUE(log.CommitReplicated(**txn, 1, payload).ok());
  EXPECT_EQ(log.last(), 1u);
  EXPECT_EQ(*ReplogAt(s, 1), payload);
  EXPECT_EQ(log.cum_bytes(), payload.size());
}

TEST(ReplLogTest, WaitBeyondWakesOnCommitAndTimesOut) {
  InMemoryStorage s;
  FakeClock clock;
  ReplLog log(s, clock);
  ASSERT_TRUE(log.Recover().ok());
  const auto start = std::chrono::steady_clock::now();
  auto waiter = std::async(std::launch::async, [&] { return log.WaitBeyond(0, std::chrono::seconds(5)); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  ASSERT_TRUE(CommitPuts(s, log, MetaPut("a")).ok());
  EXPECT_EQ(waiter.get(), 1u);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));

  const auto before = std::chrono::steady_clock::now();
  EXPECT_EQ(log.WaitBeyond(1, std::chrono::milliseconds(50)), 1u);
  EXPECT_GE(std::chrono::steady_clock::now() - before, std::chrono::milliseconds(50));
}

TEST(ReplLogTest, ConcurrentCommitsProduceAGapFreeSequence) {
  InMemoryStorage s;
  FakeClock clock;
  ReplLog log(s, clock);
  ASSERT_TRUE(log.Recover().ok());
  constexpr int kThreads = 8;
  constexpr int kPerThread = 500;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        const std::string key = "t" + std::to_string(t) + "_" + std::to_string(i);
        EXPECT_TRUE(CommitPuts(s, log, MetaPut(key, key + "v")).ok());
      }
    });
  }
  for (std::thread& th : threads) th.join();
  constexpr uint64_t kTotal = kThreads * kPerThread;
  EXPECT_EQ(log.last(), kTotal);
  auto view = s.BeginRead();
  auto it = (*view)->NewIterator(ColumnFamily::kReplog);
  ASSERT_TRUE(it.ok());
  ASSERT_TRUE((*it)->SeekToFirst().ok());
  uint64_t expected = 1;
  while ((*it)->Valid()) {
    uint64_t seq = 0;
    ASSERT_TRUE(encoding::DecodeReplogKey((*it)->key(), &seq));
    ASSERT_EQ(seq, expected++);
    ASSERT_TRUE((*it)->Next().ok());
  }
  EXPECT_EQ(expected, kTotal + 1);
  std::mt19937 rng(42);
  std::uniform_int_distribution<uint64_t> pick(1, kTotal);
  for (int n = 0; n < 50; ++n) {
    auto batch =
        encoding::DecodeReplogBatch(*(*view)->Get(ColumnFamily::kReplog, encoding::ReplogKey(pick(rng))));
    ASSERT_TRUE(batch.ok());
    ASSERT_EQ(batch->ops.size(), 1u);
    EXPECT_EQ(*(*view)->Get(ColumnFamily::kMeta, batch->ops[0].key), batch->ops[0].value);
  }
}

TEST(ReplLogTest, CommitStandaloneAppliesEveryOpType) {
  InMemoryStorage s;
  FakeClock clock;
  ReplLog log(s, clock);
  ASSERT_TRUE(log.Recover().ok());
  ASSERT_TRUE(CommitPuts(s, log,
                         {{ReplOpType::kPut, ColumnFamily::kMeta, "a1", "v"},
                          {ReplOpType::kPut, ColumnFamily::kMeta, "b1", "v"},
                          {ReplOpType::kPut, ColumnFamily::kMeta, "c1", "v"},
                          {ReplOpType::kPut, ColumnFamily::kMeta, "d1", "v"}})
                  .ok());
  const std::vector<ReplOp> ops = {{ReplOpType::kDelete, ColumnFamily::kMeta, "a1", ""},
                                   {ReplOpType::kDeletePrefix, ColumnFamily::kMeta, "b", ""},
                                   {ReplOpType::kDeleteRange, ColumnFamily::kMeta, "c", "d"},
                                   {ReplOpType::kPut, ColumnFamily::kSys, "x", "y"}};
  ASSERT_TRUE(log.CommitStandalone(ops).ok());
  EXPECT_EQ(log.last(), 2u);
  auto view = s.BeginRead();
  EXPECT_TRUE((*view)->Get(ColumnFamily::kMeta, "a1").status().IsNotFound());
  EXPECT_TRUE((*view)->Get(ColumnFamily::kMeta, "b1").status().IsNotFound());
  EXPECT_TRUE((*view)->Get(ColumnFamily::kMeta, "c1").status().IsNotFound());
  EXPECT_EQ(*(*view)->Get(ColumnFamily::kMeta, "d1"), "v");
  EXPECT_EQ(*(*view)->Get(ColumnFamily::kSys, "x"), "y");
  EXPECT_EQ(encoding::DecodeReplogBatch(*ReplogAt(s, 2))->ops, ops);
}

TEST(ReplLogTest, CommitStandaloneFencedIsRefusedAndWritesNothing) {
  InMemoryStorage s;
  FakeClock clock;
  ReplLog log(s, clock);
  ASSERT_TRUE(log.Recover().ok());
  log.FenceLocalWrites(true);
  EXPECT_EQ(log.CommitStandalone(MetaPut("a")).code(), StatusCode::kUserError);
  auto view = s.BeginRead();
  EXPECT_TRUE((*view)->Get(ColumnFamily::kMeta, "a").status().IsNotFound());
}

}  // namespace
}  // namespace tideskv
