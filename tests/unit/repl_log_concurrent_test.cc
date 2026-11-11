// A dormant ReplLog draws each commit's seq under its lock, commits outside it and publishes last() and
// cum_bytes() strictly in seq order; the head-carrying commit and an active log stay serialized.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/coding.h"
#include "encoding/replog_codec.h"
#include "exec/repl_log.h"
#include "exec/thread_commit_seq.h"
#include "storage/memory_storage.h"
#include "support/fake_clock.h"
#include "support/faulty_storage.h"
#include "support/latched_storage.h"
#include "types/errors.h"

namespace tideskv {
namespace {

using encoding::ReplOp;
using encoding::ReplOpType;
using std::chrono::milliseconds;
using test::FakeClock;
using test::FaultyStorage;
using test::LatchedStorage;

constexpr bool kLazy = true;
constexpr milliseconds kShort(50);   // long enough for a thread that is not blocked to get through
constexpr milliseconds kLong(5000);  // a bound for what must finish, so a regression fails, not hangs

std::vector<ReplOp> MetaPut(const std::string& key) {
  return {{ReplOpType::kPut, ColumnFamily::kMeta, key, "v"}};
}

Status CommitKey(Storage& s, ReplLog& log, const std::string& key) {
  const std::vector<ReplOp> ops = MetaPut(key);
  auto txn = s.BeginWrite();
  if (!txn.ok()) return txn.status();
  if (Status st = (*txn)->Put(ColumnFamily::kMeta, key, "v"); !st.ok()) return st;
  return log.Commit(**txn, ops);
}

std::future<Status> CommitKeyAsync(Storage& s, ReplLog& log, const std::string& key) {
  return std::async(std::launch::async, [&s, &log, key] { return CommitKey(s, log, key); });
}

bool Ready(std::future<Status>& f, milliseconds within) {
  return f.wait_for(within) == std::future_status::ready;
}

std::vector<uint64_t> ReplogSeqs(Storage& s) {
  std::vector<uint64_t> seqs;
  auto view = s.BeginRead();
  auto it = (*view)->NewIterator(ColumnFamily::kReplog);
  EXPECT_TRUE((*it)->SeekToFirst().ok());
  while ((*it)->Valid()) {
    uint64_t seq = 0;
    EXPECT_TRUE(encoding::DecodeReplogKey((*it)->key(), &seq));
    seqs.push_back(seq);
    if (!(*it)->Next().ok()) break;
  }
  return seqs;
}

// sys/repl_head as (seq, cum); (0, 0) when absent.
std::pair<uint64_t, uint64_t> HeadOnDisk(Storage& s) {
  auto view = s.BeginRead();
  auto raw = (*view)->Get(ColumnFamily::kSys, kSysReplHead);
  if (!raw.ok()) return {0, 0};
  std::string_view in = *raw;
  uint64_t seq = 0;
  uint64_t cum = 0;
  EXPECT_EQ(in.size(), 16u);
  EXPECT_TRUE(coding::GetFixed64BE(&in, &seq));
  EXPECT_TRUE(coding::GetFixed64BE(&in, &cum));
  return {seq, cum};
}

// A lazy log on LatchedStorage after commit #1 (the head-carrying one), with commit #2 (A) parked.
class ParkedSecondCommit : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(log_.Recover().ok());
    ASSERT_TRUE(CommitKey(s_, log_, "k1").ok());
    cum_after_1_ = log_.cum_bytes();
    s_.Hold(2);
    a_ = CommitKeyAsync(s_, log_, "a");
    ASSERT_TRUE(s_.WaitParkedFor(2, kLong));
  }
  void TearDown() override {
    s_.Release(2);
    if (a_.valid()) a_.wait();
  }

  LatchedStorage s_;
  FakeClock clock_;
  ReplLog log_{s_, clock_, kLazy};
  uint64_t cum_after_1_ = 0;
  std::future<Status> a_;
};

TEST_F(ParkedSecondCommit, DormantCommitsRunOutsideTheLock) {
  auto b = CommitKeyAsync(s_, log_, "b");
  if (!Ready(b, kLong)) {
    s_.Release(2);  // let B finish before its future is destroyed
    FAIL() << "B waited for the parked A";
  }
  EXPECT_TRUE(b.get().ok());
  EXPECT_EQ(log_.last(), 1u);  // B's seq 3 is done but waits for A's 2
  s_.Release(2);
  EXPECT_TRUE(a_.get().ok());
  EXPECT_EQ(log_.last(), 3u);
  EXPECT_EQ(log_.cum_bytes(),
            cum_after_1_ + encoding::ReplogBatchSize(MetaPut("a")) + encoding::ReplogBatchSize(MetaPut("b")));
}

TEST_F(ParkedSecondCommit, WaitBeyondNeverPassesAnUnfinishedSeq) {
  auto b = CommitKeyAsync(s_, log_, "b");
  if (!Ready(b, kLong)) {
    s_.Release(2);  // let B finish before its future is destroyed
    FAIL() << "B waited for the parked A";
  }
  EXPECT_TRUE(b.get().ok());
  EXPECT_EQ(log_.WaitBeyond(1, kShort), 1u);
  EXPECT_EQ(log_.cum_bytes(), cum_after_1_);
}

TEST_F(ParkedSecondCommit, FenceWaitsForInFlightDormantCommits) {
  auto fence = std::async(std::launch::async, [&] { log_.FenceLocalWrites(true); });
  EXPECT_EQ(fence.wait_for(kShort), std::future_status::timeout);
  s_.Release(2);
  ASSERT_EQ(fence.wait_for(kLong), std::future_status::ready);
  EXPECT_TRUE(a_.get().ok());
  EXPECT_EQ(log_.last(), 2u);
  const Status st = CommitKey(s_, log_, "late");
  EXPECT_EQ(st.code(), StatusCode::kUserError);
  EXPECT_EQ(st.message(), kErrReadOnlyReplica);
}

TEST_F(ParkedSecondCommit, ActivateAndSnapshotWaitsForInFlightCommits) {
  auto snap = std::async(std::launch::async, [&] { return log_.ActivateAndSnapshot(); });
  EXPECT_EQ(snap.wait_for(kShort), std::future_status::timeout);
  s_.Release(2);
  ASSERT_EQ(snap.wait_for(kLong), std::future_status::ready);
  auto got = snap.get();
  ASSERT_TRUE(got.ok()) << got.status().message();
  EXPECT_EQ(got->seq, 2u);
  EXPECT_TRUE(got->view->Get(ColumnFamily::kMeta, "a").ok());
  EXPECT_TRUE(a_.get().ok());
  EXPECT_EQ(ReplogSeqs(s_), (std::vector<uint64_t>{2}));  // the baseline
}

TEST_F(ParkedSecondCommit, PersistHeadDoesNotWaitForInFlightCommits) {
  auto persist = std::async(std::launch::async, [&] { return log_.PersistHead(); });
  if (persist.wait_for(kShort) != std::future_status::ready) {
    s_.Release(2);
    FAIL() << "PersistHead waited for A";
  }
  EXPECT_TRUE(persist.get().ok());
  EXPECT_EQ(HeadOnDisk(s_), (std::pair<uint64_t, uint64_t>{1, cum_after_1_}));
}

TEST(ReplLogConcurrentTest, AFailedMiddleCommitIsSkipped) {
  LatchedStorage s;
  FakeClock clock;
  ReplLog log(s, clock, kLazy);
  ASSERT_TRUE(log.Recover().ok());
  ASSERT_TRUE(CommitKey(s, log, "k1").ok());
  const uint64_t cum_after_1 = log.cum_bytes();
  s.Hold(2);
  s.Hold(3);
  auto a = CommitKeyAsync(s, log, "a");
  ASSERT_TRUE(s.WaitParkedFor(2, kLong));
  auto b = CommitKeyAsync(s, log, "b");
  const bool b_parked = s.WaitParkedFor(3, kLong);
  s.Release(2, false);
  s.Release(3, true);
  ASSERT_TRUE(b_parked) << "B never reached storage while A was parked";
  EXPECT_EQ(a.get().code(), StatusCode::kIOError);
  EXPECT_TRUE(b.get().ok());
  EXPECT_EQ(log.last(), 3u);
  EXPECT_EQ(log.cum_bytes(), cum_after_1 + encoding::ReplogBatchSize(MetaPut("b")));
  ASSERT_TRUE(CommitKey(s, log, "c").ok());
  EXPECT_EQ(ThreadLastCommitSeq(), 4u);
}

TEST(ReplLogConcurrentTest, AFailedNewestCommitConsumesNoSeq) {
  FaultyStorage s;
  FakeClock clock;
  ReplLog log(s, clock, kLazy);
  ASSERT_TRUE(log.Recover().ok());
  ASSERT_TRUE(CommitKey(s, log, "k1").ok());
  const uint64_t cum_after_1 = log.cum_bytes();
  s.fail_commit_in = 1;
  EXPECT_FALSE(CommitKey(s, log, "bad").ok());
  EXPECT_EQ(log.last(), 1u);
  EXPECT_EQ(log.cum_bytes(), cum_after_1);
  ASSERT_TRUE(CommitKey(s, log, "good").ok());
  EXPECT_EQ(ThreadLastCommitSeq(), 2u);
  EXPECT_EQ(log.last(), 2u);
}

TEST(ReplLogConcurrentTest, TheHeadCommitRunsSerialized) {
  LatchedStorage s;
  FakeClock clock;
  ReplLog log(s, clock, kLazy);
  ASSERT_TRUE(log.Recover().ok());
  s.Hold(1);
  auto a = CommitKeyAsync(s, log, "a");
  ASSERT_TRUE(s.WaitParkedFor(1, kLong));
  auto b = CommitKeyAsync(s, log, "b");
  std::this_thread::sleep_for(kShort);
  EXPECT_EQ(s.reached(), 1);
  s.Release(1);
  EXPECT_TRUE(a.get().ok());
  EXPECT_TRUE(b.get().ok());
  EXPECT_EQ(log.last(), 2u);
  EXPECT_EQ(HeadOnDisk(s).first, 1u);
}

TEST(ReplLogConcurrentTest, AFailedHeadCommitLeavesTheNextOneToCarryTheHead) {
  FaultyStorage s;
  FakeClock clock;
  ReplLog log(s, clock, kLazy);
  ASSERT_TRUE(log.Recover().ok());
  s.fail_commit_in = 1;
  EXPECT_FALSE(CommitKey(s, log, "bad").ok());
  ASSERT_TRUE(CommitKey(s, log, "good").ok());
  EXPECT_EQ(ThreadLastCommitSeq(), 1u);
  const auto head = HeadOnDisk(s);
  EXPECT_EQ(head.first, 1u);  // present, and the seq #2 took after the rollback
  EXPECT_EQ(head.second, log.cum_bytes());
}

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define TIDESKV_UNIT_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define TIDESKV_UNIT_SANITIZED 1
#endif
#endif

TEST(ReplLogConcurrentTest, ConcurrentDormantCommitsAreGapFreeAndCumIsExact) {
  InMemoryStorage s;
  FakeClock clock;
  ReplLog log(s, clock, kLazy);
  ASSERT_TRUE(log.Recover().ok());
  constexpr int kThreads = 8;
  // InMemoryStorage copies every table on each commit (see repl_log_test.cc's gap-free test).
#ifdef TIDESKV_UNIT_SANITIZED
  constexpr int kPerThread = 150;
#else
  constexpr int kPerThread = 500;
#endif
  std::vector<uint64_t> sizes(kThreads, 0);
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      uint64_t prev = 0;
      for (int i = 0; i < kPerThread; ++i) {
        const std::string key = "t" + std::to_string(t) + "_" + std::to_string(i);
        ASSERT_TRUE(CommitKey(s, log, key).ok());
        sizes[t] += encoding::ReplogBatchSize(MetaPut(key));
        EXPECT_GT(ThreadLastCommitSeq(), prev);
        prev = ThreadLastCommitSeq();
      }
    });
  }
  for (std::thread& th : threads) th.join();
  uint64_t total_size = 0;
  for (uint64_t size : sizes) total_size += size;
  EXPECT_EQ(log.last(), static_cast<uint64_t>(kThreads) * kPerThread);
  EXPECT_EQ(log.first_retained(), log.last() + 1);
  EXPECT_EQ(log.cum_bytes(), total_size);
}

TEST(ReplLogConcurrentTest, AnActiveLogStillSerializesCommits) {
  LatchedStorage s;
  FakeClock clock;
  ReplLog log(s, clock);  // not lazy: always active
  ASSERT_TRUE(log.Recover().ok());
  s.Hold(1);
  auto a = CommitKeyAsync(s, log, "a");
  ASSERT_TRUE(s.WaitParkedFor(1, kLong));
  auto b = CommitKeyAsync(s, log, "b");
  std::this_thread::sleep_for(kShort);
  EXPECT_EQ(s.reached(), 1);
  s.Release(1);
  EXPECT_TRUE(a.get().ok());
  EXPECT_TRUE(b.get().ok());
  EXPECT_EQ(log.last(), 2u);
  EXPECT_EQ(ReplogSeqs(s), (std::vector<uint64_t>{1, 2}));
}

}  // namespace
}  // namespace tideskv
