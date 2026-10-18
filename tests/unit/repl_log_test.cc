#include "exec/repl_log.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
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

// CF2: the master's cum_bytes must continue ours (cum_ + payload size), from the very first batch.
TEST(ReplLogTest, CommitReplicatedRefusesAMisalignedCumBytes) {
  InMemoryStorage s;
  FakeClock clock;
  ReplLog log(s, clock);
  ASSERT_TRUE(log.Recover().ok());
  const auto refused = [&](uint64_t seq, const std::string& payload) {
    auto txn = s.BeginWrite();
    EXPECT_TRUE((*txn)->Put(ColumnFamily::kMeta, "x", "v").ok());
    const Status st = log.CommitReplicated(**txn, seq, payload);
    EXPECT_EQ(st.code(), StatusCode::kCorruption) << st.ToString();
  };
  refused(1, encoding::EncodeReplogBatch(MetaPut("a"), 1, 7));  // an empty replica starts at 0
  EXPECT_EQ(log.last(), 0u);
  EXPECT_EQ(log.cum_bytes(), 0u);
  EXPECT_FALSE(ReplogAt(s, 1).ok());
  const std::string first = encoding::EncodeReplogBatch(MetaPut("a"), 1, 0);
  {
    auto txn = s.BeginWrite();
    ASSERT_TRUE(log.CommitReplicated(**txn, 1, first).ok());
  }
  refused(2, encoding::EncodeReplogBatch(MetaPut("b"), 2, first.size() + 1));
  refused(2, encoding::EncodeReplogBatch(MetaPut("b"), 2, first.size() - 1));
  EXPECT_EQ(log.last(), 1u);
  EXPECT_EQ(log.cum_bytes(), first.size());
  auto view = s.BeginRead();
  EXPECT_TRUE((*view)->Get(ColumnFamily::kMeta, "x").status().IsNotFound());  // refused txns not committed
  auto txn = s.BeginWrite();
  const std::string second = encoding::EncodeReplogBatch(MetaPut("b"), 2, first.size());
  ASSERT_TRUE(log.CommitReplicated(**txn, 2, second).ok());
  EXPECT_EQ(log.cum_bytes(), first.size() + second.size());
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

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define TIDESKV_UNIT_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define TIDESKV_UNIT_SANITIZED 1
#endif
#endif

TEST(ReplLogTest, ConcurrentCommitsProduceAGapFreeSequence) {
  InMemoryStorage s;
  FakeClock clock;
  ReplLog log(s, clock);
  ASSERT_TRUE(log.Recover().ok());
  constexpr int kThreads = 8;
  // InMemoryStorage copies every table on each commit, so this test costs O(commits^2) node copies, all
  // under the storage mutex. 4000 commits take 1 s in release, 10 s (ASan) and 15 s (TSan) alone on 4
  // cores, 45 s beside three heavy tests, and over 60 s on a 4-vCPU CI runner; 1200 commits (8 x 150)
  // still interleave every thread and take 1.9 s alone under either sanitizer.
#ifdef TIDESKV_UNIT_SANITIZED
  constexpr int kPerThread = 150;
#else
  constexpr int kPerThread = 500;
#endif
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

// R12 (F2): a batch a replica could not accept is never committed. One shared limit; the default is
// encoding::kMaxReplBatchBytes and tests lower it rather than allocate 1 GiB.
TEST(ReplLogTest, ABatchOverTheReplicationLimitIsRefusedAndConsumesNoSeq) {
  InMemoryStorage s;
  FakeClock clock;
  ReplLog log(s, clock);
  ASSERT_TRUE(log.Recover().ok());
  EXPECT_EQ(log.max_batch_bytes(), encoding::kMaxReplBatchBytes);
  ASSERT_TRUE(CommitPuts(s, log, MetaPut("small")).ok());
  const uint64_t cum = log.cum_bytes();
  log.SetMaxBatchBytes(200);
  const Status st = CommitPuts(s, log, MetaPut("big", std::string(300, 'x')));
  EXPECT_EQ(st.code(), StatusCode::kUserError);
  EXPECT_EQ(st.message(), kErrTooLargeToReplicate);
  EXPECT_EQ(log.CommitStandalone(MetaPut("big2", std::string(300, 'x'))).message(), kErrTooLargeToReplicate);
  EXPECT_EQ(log.last(), 1u);
  EXPECT_EQ(log.cum_bytes(), cum);
  {
    auto view = s.BeginRead();
    EXPECT_TRUE((*view)->Get(ColumnFamily::kMeta, "big").status().IsNotFound());
    EXPECT_TRUE((*view)->Get(ColumnFamily::kMeta, "big2").status().IsNotFound());
  }
  EXPECT_TRUE(ReplogAt(s, 2).status().IsNotFound());
  ASSERT_TRUE(CommitPuts(s, log, MetaPut("next")).ok());  // within the limit: the next seq
  EXPECT_EQ(log.last(), 2u);
}

}  // namespace
}  // namespace tideskv

namespace tideskv {
namespace {

// InMemoryStorage whose transactions park in Commit() while `hold` is set, so a test can freeze a
// local commit inside ReplLog's commit section.
class ParkingStorage final : public Storage {
 public:
  std::atomic<bool> hold{false};
  std::atomic<int> parked{0};

  Result<std::unique_ptr<WriteTxn>> BeginWrite() override {
    auto txn = inner_.BeginWrite();
    if (!txn.ok()) return txn.status();
    return std::unique_ptr<WriteTxn>(std::make_unique<Txn>(std::move(txn).value(), *this));
  }
  Result<std::unique_ptr<ReadView>> BeginRead() override { return inner_.BeginRead(); }
  Result<std::unique_ptr<ReadView>> BeginSnapshot() override { return inner_.BeginSnapshot(); }

 private:
  class Txn final : public WriteTxn {
   public:
    Txn(std::unique_ptr<WriteTxn> inner, ParkingStorage& owner) : inner_(std::move(inner)), owner_(owner) {}
    Result<std::string> Get(ColumnFamily cf, std::string_view k) override { return inner_->Get(cf, k); }
    Result<std::unique_ptr<Iterator>> NewIterator(ColumnFamily cf, IterOptions o) override {
      return inner_->NewIterator(cf, std::move(o));
    }
    Status Put(ColumnFamily cf, std::string_view k, std::string_view v) override {
      return inner_->Put(cf, k, v);
    }
    Status Delete(ColumnFamily cf, std::string_view k) override { return inner_->Delete(cf, k); }
    Status DeletePrefix(ColumnFamily cf, std::string_view p) override { return inner_->DeletePrefix(cf, p); }
    Status DeleteRange(ColumnFamily cf, std::string_view lo, std::string_view hi) override {
      return inner_->DeleteRange(cf, lo, hi);
    }
    Status Savepoint(std::string_view n) override { return inner_->Savepoint(n); }
    Status RollbackToSavepoint(std::string_view n) override { return inner_->RollbackToSavepoint(n); }
    Status ReleaseSavepoint(std::string_view n) override { return inner_->ReleaseSavepoint(n); }
    Status Commit() override {
      ++owner_.parked;
      while (owner_.hold.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      return inner_->Commit();
    }
    Status Rollback() override { return inner_->Rollback(); }

   private:
    std::unique_ptr<WriteTxn> inner_;
    ParkingStorage& owner_;
  };

  InMemoryStorage inner_;
};

// CF1: once FenceLocalWrites(true) returns, no local commit can take a seq, so the first replicated
// apply always finds seq == last()+1. A commit already inside the commit section finishes first.
TEST(ReplLogTest, RaisingTheFenceWaitsForAnInFlightLocalCommit) {
  ParkingStorage s;
  FakeClock clock;
  ReplLog log(s, clock);
  ASSERT_TRUE(log.Recover().ok());
  s.hold = true;
  auto local = std::async(std::launch::async, [&] { return CommitPuts(s, log, MetaPut("a")); });
  while (s.parked.load() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  std::atomic<bool> fenced{false};
  std::thread fencer([&] {
    log.FenceLocalWrites(true);
    fenced = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(fenced.load()) << "the fence returned while a local commit was in flight";
  s.hold = false;
  fencer.join();
  EXPECT_TRUE(local.get().ok());
  EXPECT_EQ(log.last(), 1u);
  const Status after = CommitPuts(s, log, MetaPut("b"));
  EXPECT_EQ(after.code(), StatusCode::kUserError);
  EXPECT_EQ(log.last(), 1u);
}

// CF1: a local commit that passed every unlocked check before the fence rose, and now waits for the
// commit section, must not take a seq once FenceLocalWrites(true) has returned.
TEST(ReplLogTest, NoLocalCommitLandsAfterTheFenceReturns) {
  for (int round = 0; round < 20; ++round) {
    ParkingStorage s;
    FakeClock clock;
    ReplLog log(s, clock);
    ASSERT_TRUE(log.Recover().ok());
    s.hold = true;
    auto first = std::async(std::launch::async, [&] { return CommitPuts(s, log, MetaPut("a")); });
    while (s.parked.load() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto second = std::async(std::launch::async, [&] { return CommitPuts(s, log, MetaPut("b")); });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));  // second now waits for the section
    std::atomic<uint64_t> last_at_fence{UINT64_MAX};
    std::thread fencer([&] {
      log.FenceLocalWrites(true);
      last_at_fence = log.last();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    s.hold = false;
    fencer.join();
    EXPECT_TRUE(first.get().ok());
    const Status st = second.get();
    EXPECT_TRUE(st.ok() || st.code() == StatusCode::kUserError) << st.ToString();
    EXPECT_EQ(log.last(), last_at_fence.load()) << "a local commit landed after the fence returned";
    EXPECT_EQ(CommitPuts(s, log, MetaPut("c")).code(), StatusCode::kUserError);
  }
}

// CF3: a stored first retained seq must be in [1, last + 1].
TEST(ReplLogTest, RecoverRejectsAFirstOutsideOneToLastPlusOne) {
  for (const uint64_t stored : {uint64_t{0}, uint64_t{1}, uint64_t{4}, uint64_t{5}, uint64_t{100}}) {
    InMemoryStorage s;
    FakeClock clock;
    {
      ReplLog log(s, clock);
      ASSERT_TRUE(log.Recover().ok());
      for (int i = 1; i <= 3; ++i) ASSERT_TRUE(CommitPuts(s, log, MetaPut("k" + std::to_string(i))).ok());
    }
    std::string first;
    coding::PutFixed64BE(&first, stored);
    auto txn = s.BeginWrite();
    ASSERT_TRUE((*txn)->Put(ColumnFamily::kSys, "repl_first", first).ok());
    ASSERT_TRUE((*txn)->Commit().ok());
    ReplLog log(s, clock);
    const Status st = log.Recover();
    if (stored >= 1 && stored <= 4) {  // last + 1 = 4: everything was trimmed (a full sync baseline)
      EXPECT_TRUE(st.ok()) << stored << ": " << st.ToString();
      EXPECT_EQ(log.first_retained(), stored);
    } else {
      EXPECT_EQ(st.code(), StatusCode::kCorruption) << stored;
    }
  }
}

// CF3: the newest batch is never trimmed, whatever the caller asks for.
TEST(ReplLogTest, TrimRefusesTheNewestBatch) {
  InMemoryStorage s;
  FakeClock clock;
  ReplLog log(s, clock);
  ASSERT_TRUE(log.Recover().ok());
  EXPECT_EQ(log.Trim(0).code(), StatusCode::kInvalidArgument);  // empty log: nothing may go
  for (int i = 1; i <= 4; ++i) ASSERT_TRUE(CommitPuts(s, log, MetaPut("k" + std::to_string(i))).ok());
  EXPECT_EQ(log.Trim(4).code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(log.Trim(9).code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(log.first_retained(), 1u);
  EXPECT_TRUE(ReplogAt(s, 1).ok());
  ASSERT_TRUE(log.Trim(3).ok());
  EXPECT_EQ(log.first_retained(), 4u);
  ASSERT_TRUE(log.Trim(2).ok());  // below first: nothing to do
  EXPECT_EQ(log.first_retained(), 4u);
  EXPECT_TRUE(ReplogAt(s, 4).ok());
  ASSERT_TRUE(CommitPuts(s, log, MetaPut("k5")).ok());  // commits continue after a trim
  EXPECT_EQ(log.last(), 5u);
}

TEST(ReplLogTest, TrimUnlessFencedDoesNothingOnceFenced) {
  InMemoryStorage s;
  FakeClock clock;
  ReplLog log(s, clock);
  ASSERT_TRUE(log.Recover().ok());
  for (int i = 1; i <= 4; ++i) ASSERT_TRUE(CommitPuts(s, log, MetaPut("k" + std::to_string(i))).ok());
  bool trimmed = true;
  log.FenceLocalWrites(true);
  ASSERT_TRUE(log.TrimUnlessFenced(2, &trimmed).ok());
  EXPECT_FALSE(trimmed);
  EXPECT_EQ(log.first_retained(), 1u);
  EXPECT_TRUE(ReplogAt(s, 1).ok());
  ASSERT_TRUE(log.Trim(1).ok());  // the replica's applier is not fenced out
  EXPECT_EQ(log.first_retained(), 2u);
  log.FenceLocalWrites(false);
  ASSERT_TRUE(log.TrimUnlessFenced(2, &trimmed).ok());
  EXPECT_TRUE(trimmed);
  EXPECT_EQ(log.first_retained(), 3u);
}

}  // namespace
}  // namespace tideskv
