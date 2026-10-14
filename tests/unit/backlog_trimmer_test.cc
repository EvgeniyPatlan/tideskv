#include "repl/backlog_trimmer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "common/coding.h"
#include "encoding/replog_codec.h"
#include "repl/repl_state.h"
#include "support/fake_clock.h"
#include "support/faulty_storage.h"

namespace tideskv::repl {
namespace {

using namespace std::chrono_literals;
using encoding::ReplOp;
using encoding::ReplOpType;
using test::FakeClock;
using test::FaultyStorage;

bool WaitFor(const std::function<bool()>& cond, std::chrono::milliseconds timeout = 5s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (cond()) return true;
    std::this_thread::sleep_for(2ms);
  }
  return cond();
}

// ---- ComputeTrimPoint ------------------------------------------------------------------------------

// Ten 100-byte batches: cum(s) = 100 s, timestamp(s) = 1000 s ms.
constexpr uint64_t kBatch = 100;
TrimInputs TenBatches(uint64_t first = 1) {
  TrimInputs in;
  in.first = first;
  in.last = 10;
  in.first_cum = kBatch * first;
  in.first_size = kBatch;
  in.first_ts = static_cast<int64_t>(1000 * first);
  in.last_cum = kBatch * 10;
  in.now_ms = 10'000;
  in.backlog_size = 1'000'000;
  in.backlog_ttl_s = 0;
  in.peek = [](uint64_t seq) {
    return std::optional<ReplogHeader>(ReplogHeader{static_cast<int64_t>(1000 * seq), kBatch * seq});
  };
  return in;
}

TEST(ComputeTrimPointTest, NeitherLimitReached) {
  EXPECT_EQ(ComputeTrimPoint(TenBatches()), std::nullopt);
  TrimInputs in = TenBatches();
  in.backlog_size = 1000;  // exactly the ten batches
  in.backlog_ttl_s = 10;   // batch 1 (ts 1000) is not older than now - 10 s = 0
  EXPECT_EQ(ComputeTrimPoint(in), std::nullopt);
}

TEST(ComputeTrimPointTest, SizeLimit) {
  TrimInputs in = TenBatches();
  in.backlog_size = 450;  // 7..10 (400 bytes) fit, 6..10 (500) do not
  EXPECT_EQ(ComputeTrimPoint(in), 6u);
  in.backlog_size = 999;  // 2..10
  EXPECT_EQ(ComputeTrimPoint(in), 1u);
  in = TenBatches(7);  // already trimmed to 7: 400 bytes retained
  in.backlog_size = 450;
  EXPECT_EQ(ComputeTrimPoint(in), std::nullopt);
  in = TenBatches(4);
  in.backlog_size = 450;
  EXPECT_EQ(ComputeTrimPoint(in), 6u);
}

TEST(ComputeTrimPointTest, TtlLimit) {
  TrimInputs in = TenBatches();
  in.backlog_ttl_s = 4;  // older than 6000 ms: 1..5
  EXPECT_EQ(ComputeTrimPoint(in), 5u);
  in.now_ms = 6'001;  // older than 2001 ms: 1..2
  EXPECT_EQ(ComputeTrimPoint(in), 2u);
}

TEST(ComputeTrimPointTest, BothLimitsTheFurtherWins) {
  TrimInputs in = TenBatches();
  in.backlog_ttl_s = 4;   // -> 5
  in.backlog_size = 450;  // -> 6
  EXPECT_EQ(ComputeTrimPoint(in), 6u);
  in.backlog_size = 750;  // -> 3
  EXPECT_EQ(ComputeTrimPoint(in), 5u);
}

TEST(ComputeTrimPointTest, AReplicaWithinTheLimitsHoldsTheAgeTrimBack) {
  TrimInputs in = TenBatches();
  in.backlog_ttl_s = 4;  // -> 5
  in.oldest_needed = 3;  // the replica still needs 3
  EXPECT_EQ(ComputeTrimPoint(in), 2u);
  in.oldest_needed = 1;
  EXPECT_EQ(ComputeTrimPoint(in), std::nullopt);
  in.oldest_needed = 9;  // needs nothing the age trim deletes
  EXPECT_EQ(ComputeTrimPoint(in), 5u);
}

TEST(ComputeTrimPointTest, ALaggingReplicaBeyondTheSizeLimitDoesNotHoldItBack) {
  TrimInputs in = TenBatches();
  in.backlog_size = 450;  // -> 6
  in.oldest_needed = 3;   // beyond the limit: the caller drops it
  EXPECT_EQ(ComputeTrimPoint(in), 6u);
  in.backlog_ttl_s = 4;  // the age trim (5) stays held back at 2; the size trim still wins
  EXPECT_EQ(ComputeTrimPoint(in), 6u);
}

TEST(ComputeTrimPointTest, TheNewestBatchIsNeverTrimmed) {
  TrimInputs in = TenBatches();
  in.backlog_size = 1;  // smaller than one batch
  EXPECT_EQ(ComputeTrimPoint(in), 9u);
  in.backlog_size = 1'000'000;
  in.backlog_ttl_s = 1;
  in.now_ms = 1'000'000;  // every batch is old
  EXPECT_EQ(ComputeTrimPoint(in), 9u);
  in = TenBatches(10);  // only the newest is left
  in.backlog_size = 1;
  EXPECT_EQ(ComputeTrimPoint(in), std::nullopt);
  TrimInputs empty;
  empty.backlog_size = 1;
  EXPECT_EQ(ComputeTrimPoint(empty), std::nullopt);
}

TEST(ComputeTrimPointTest, AnUnreadableHeaderTrimsNothing) {
  TrimInputs in = TenBatches();
  in.backlog_size = 450;
  in.peek = [](uint64_t) { return std::optional<ReplogHeader>(); };
  EXPECT_EQ(ComputeTrimPoint(in), std::nullopt);
}

// ---- ReplLog::Trim ---------------------------------------------------------------------------------

std::vector<ReplOp> Put(int i) {
  return {{ReplOpType::kPut, ColumnFamily::kMeta, "k" + std::to_string(i), "v"}};
}

void CommitN(ReplLog& log, int n) {
  for (int i = 0; i < n; ++i) ASSERT_TRUE(log.CommitStandalone(Put(i)).ok());
}

bool Present(Storage& s, uint64_t seq) {
  auto view = s.BeginRead();
  return (*view)->Get(ColumnFamily::kReplog, encoding::ReplogKey(seq)).ok();
}

TEST(BacklogTrimTest, TrimDeletesAndPersistsFirst) {
  FaultyStorage s;
  FakeClock clock;
  ReplLog log(s, clock);
  ASSERT_TRUE(log.Recover().ok());
  CommitN(log, 10);
  const uint64_t cum = log.cum_bytes();
  ASSERT_TRUE(log.Trim(5).ok());
  for (uint64_t seq = 1; seq <= 5; ++seq) EXPECT_FALSE(Present(s, seq)) << seq;
  for (uint64_t seq = 6; seq <= 10; ++seq) EXPECT_TRUE(Present(s, seq)) << seq;
  EXPECT_EQ(log.first_retained(), 6u);
  auto view = s.BeginRead();
  auto raw = (*view)->Get(ColumnFamily::kSys, kSysReplFirst);
  ASSERT_TRUE(raw.ok());
  std::string_view in = *raw;
  uint64_t first = 0;
  ASSERT_TRUE(coding::GetFixed64BE(&in, &first));
  EXPECT_EQ(first, 6u);
  ReplLog again(s, clock);
  ASSERT_TRUE(again.Recover().ok());
  EXPECT_EQ(again.first_retained(), 6u);
  EXPECT_EQ(again.last(), 10u);
  EXPECT_EQ(again.cum_bytes(), cum);
}

TEST(BacklogTrimTest, SenderAfterTrimStartsAtFirstRetained) {
  FaultyStorage s;
  FakeClock clock;
  ReplLog log(s, clock);
  ASSERT_TRUE(log.Recover().ok());
  CommitN(log, 10);
  ASSERT_TRUE(log.Trim(5).ok());
  ReplIdentity me;
  me.replid = std::string(kReplIdLength, 'c');
  const LogBounds bounds{log.first_retained(), log.last()};
  EXPECT_EQ(DecidePsync(me, bounds, {me.replid, 3}), PsyncDecision::kFullResync);
  EXPECT_EQ(DecidePsync(me, bounds, {me.replid, 5}), PsyncDecision::kFullResync);
  EXPECT_EQ(DecidePsync(me, bounds, {me.replid, 6}), PsyncDecision::kContinue);
}

// ---- BacklogTrimmer --------------------------------------------------------------------------------

class FakeCompactor final : public Compactor {
 public:
  Status Checkpoint() override {
    ++checkpoints;
    return Status::Ok();
  }
  Status Compact(ColumnFamily cf) override {
    std::lock_guard lock(mu);
    compacted.push_back(cf);
    return busy ? Status::Busy("compacting") : Status::Ok();
  }
  size_t count() {
    std::lock_guard lock(mu);
    return compacted.size();
  }
  std::atomic<int> checkpoints{0};
  bool busy = false;
  std::mutex mu;
  std::vector<ColumnFamily> compacted;
};

struct TrimmerTest : ::testing::Test {
  TrimmerTest() { EXPECT_TRUE(log.Recover().ok()); }

  ConfigValues Config() {
    std::lock_guard lock(mu);
    return cfg;
  }
  void SetSize(uint64_t bytes) {
    std::lock_guard lock(mu);
    cfg.repl_backlog_size = bytes;
  }
  BacklogTrimmer::Hooks Hooks() {
    BacklogTrimmer::Hooks h;
    h.config = [this] { return Config(); };
    return h;
  }
  BacklogTrimmer::Options Fast() {
    BacklogTrimmer::Options o;
    o.interval = 20ms;
    return o;
  }
  // The bytes retained: the stream from first_retained() through last().
  uint64_t Retained() {
    auto view = s.BeginRead();
    auto raw = (*view)->Get(ColumnFamily::kReplog, encoding::ReplogKey(log.first_retained()));
    if (!raw.ok()) return 0;
    int64_t ts = 0;
    uint64_t cum = 0;
    EXPECT_TRUE(encoding::PeekReplogHeader(*raw, &ts, &cum));
    return log.cum_bytes() - cum + raw->size();
  }

  FaultyStorage s;
  FakeClock clock;
  ReplLog log{s, clock};
  FakeCompactor compactor;
  std::mutex mu;
  ConfigValues cfg;
};

TEST_F(TrimmerTest, TheThreadTrimsToTheSizeLimitWithinTwoTicks) {
  CommitN(log, 100);
  const uint64_t one = log.cum_bytes() / 100;
  SetSize(one * 10);
  BacklogTrimmer t(s, log, &compactor, clock, Hooks(), Fast());
  t.Start();
  ASSERT_TRUE(WaitFor([&] { return t.ticks() >= 2; }));
  EXPECT_LE(Retained(), one * 10);
  EXPECT_GT(Retained(), one * 8);
  EXPECT_EQ(log.last(), 100u);
  EXPECT_GE(log.first_retained(), 90u);
  EXPECT_EQ(t.trimmed_total(), log.first_retained() - 1);
  t.Stop();
  t.Stop();  // idempotent
}

TEST_F(TrimmerTest, TheThreadTrimsByAgeUnderAFakeClock) {
  CommitN(log, 5);
  clock.Advance(3'000);
  CommitN(log, 5);
  {
    std::lock_guard lock(mu);
    cfg.repl_backlog_ttl = 5;
  }
  BacklogTrimmer t(s, log, nullptr, clock, Hooks(), Fast());
  t.Start();
  const uint64_t before = t.ticks();
  ASSERT_TRUE(WaitFor([&] { return t.ticks() >= before + 2; }));
  EXPECT_EQ(log.first_retained(), 1u);  // nothing is 5 s old yet
  clock.Advance(6'000);                 // the first five are 9 s old, the rest 6 s: all of them
  const uint64_t now = t.ticks();
  ASSERT_TRUE(WaitFor([&] { return t.ticks() >= now + 2; }));
  EXPECT_EQ(log.first_retained(), 10u);  // the newest stays
}

TEST_F(TrimmerTest, TrimsInStepsOfAtMostMaxBatches) {
  CommitN(log, 95);
  SetSize(1);
  BacklogTrimmer::Options o = Fast();
  o.max_batches_per_trim = 10;
  BacklogTrimmer t(s, log, nullptr, clock, Hooks(), o);
  const int commits = s.commits.load();
  ASSERT_TRUE(t.RunOnce().ok());
  EXPECT_EQ(log.first_retained(), 95u);
  EXPECT_EQ(s.commits.load() - commits, 10);  // 94 batches: nine of 10, one of 4
}

TEST_F(TrimmerTest, TheMasterThreadDoesNothingWhileFenced) {
  CommitN(log, 20);
  SetSize(1);
  log.FenceLocalWrites(true);
  BacklogTrimmer t(s, log, nullptr, clock, Hooks(), Fast());
  ASSERT_TRUE(t.RunOnce().ok());
  EXPECT_EQ(log.first_retained(), 1u);
  ASSERT_TRUE(t.RunOnReplica().ok());  // the replica's applier does trim
  EXPECT_EQ(log.first_retained(), 20u);
}

TEST_F(TrimmerTest, ReplicasWithinTheLimitsHoldTheAgeTrimAndLaggingOnesAreDropped) {
  CommitN(log, 10);
  clock.Advance(100'000);
  CommitN(log, 10);
  {
    std::lock_guard lock(mu);
    cfg.repl_backlog_ttl = 50;  // 1..10 are old
  }
  std::atomic<uint64_t> needed{4};
  std::vector<uint64_t> dropped;
  BacklogTrimmer::Hooks h = Hooks();
  h.oldest_needed = [&] { return needed.load(); };
  h.drop_replicas_needing = [&](uint64_t up_to) { dropped.push_back(up_to); };
  BacklogTrimmer t(s, log, nullptr, clock, h, Fast());
  ASSERT_TRUE(t.RunOnce().ok());
  EXPECT_EQ(log.first_retained(), 4u);  // held back by the replica
  EXPECT_TRUE(dropped.empty());
  const uint64_t one = log.cum_bytes() / 20;
  SetSize(one * 5);  // 16..20
  ASSERT_TRUE(t.RunOnce().ok());
  EXPECT_EQ(log.first_retained(), 16u);
  EXPECT_EQ(dropped, (std::vector<uint64_t>{15}));  // dropped before the trim passed it
}

TEST_F(TrimmerTest, CompactsTheReplogAfterEnoughTrimsAtMostOncePerInterval) {
  BacklogTrimmer::Options o = Fast();
  o.compact_after_batches = 50;
  o.compact_min_interval = 10min;
  BacklogTrimmer t(s, log, &compactor, clock, Hooks(), o);
  SetSize(1);
  CommitN(log, 30);
  ASSERT_TRUE(t.RunOnce().ok());  // 29 trimmed
  EXPECT_EQ(compactor.count(), 0u);
  CommitN(log, 30);
  ASSERT_TRUE(t.RunOnce().ok());  // 59: compact
  ASSERT_EQ(compactor.count(), 1u);
  EXPECT_EQ(compactor.compacted[0], ColumnFamily::kReplog);
  EXPECT_EQ(t.compactions(), 1u);
  CommitN(log, 60);
  ASSERT_TRUE(t.RunOnce().ok());  // 60 more, but within ten minutes of the last
  EXPECT_EQ(compactor.count(), 1u);
  clock.Advance(10 * 60 * 1000);
  ASSERT_TRUE(t.RunOnce().ok());  // nothing new to trim; the pending count compacts now
  EXPECT_EQ(compactor.count(), 2u);
  compactor.busy = true;  // a busy CF is retried on a later round
  CommitN(log, 60);
  clock.Advance(10 * 60 * 1000);
  ASSERT_TRUE(t.RunOnce().ok());
  EXPECT_EQ(compactor.count(), 3u);
  EXPECT_EQ(t.compactions(), 2u);
  compactor.busy = false;
  ASSERT_TRUE(t.RunOnce().ok());
  EXPECT_EQ(compactor.count(), 4u);
  EXPECT_EQ(t.compactions(), 3u);
}

TEST_F(TrimmerTest, AFailedTrimIsReportedAndRetried) {
  CommitN(log, 10);
  SetSize(1);
  BacklogTrimmer t(s, log, nullptr, clock, Hooks(), Fast());
  s.fail_commit_in = 1;
  EXPECT_FALSE(t.RunOnce().ok());
  EXPECT_EQ(log.first_retained(), 1u);
  ASSERT_TRUE(t.RunOnce().ok());
  EXPECT_EQ(log.first_retained(), 10u);
}

}  // namespace
}  // namespace tideskv::repl
