#include "server/compaction_scheduler.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "encoding/key_codec.h"
#include "encoding/key_layout.h"
#include "encoding/object_codec.h"

namespace tideskv {
namespace {

struct Call {
  std::string what;  // "checkpoint" or "compact"
  ColumnFamily cf = ColumnFamily::kMeta;
};

class FakeCompactor final : public Compactor {
 public:
  Status Checkpoint() override {
    std::lock_guard lock(mu);
    calls.push_back({"checkpoint", ColumnFamily::kMeta});
    return Status::Ok();
  }
  Status Compact(ColumnFamily cf) override {
    std::lock_guard lock(mu);
    calls.push_back({"compact", cf});
    if (busy_left > 0) {
      --busy_left;
      return Status::Busy("compaction running");
    }
    return Status::Ok();
  }
  std::vector<Call> Calls() {
    std::lock_guard lock(mu);
    return calls;
  }
  std::mutex mu;
  std::vector<Call> calls;
  int busy_left = 0;
};

RangeDelete Prefix(ColumnFamily cf, std::string p) { return {cf, std::move(p), {}, true}; }

CompactionScheduler::Options Opts(uint64_t threshold) {
  CompactionScheduler::Options o;
  o.collection_delete_threshold = [threshold] { return threshold; };
  return o;
}

TEST(CompactionScheduler, FlushDbCompactsEachDeletedCfAfterACheckpoint) {
  FakeCompactor c;
  CompactionScheduler s(c, Opts(1000));
  s.OnCommitted({Prefix(ColumnFamily::kMeta, encoding::DbPrefix(3)),
                 Prefix(ColumnFamily::kSubkey, encoding::DbPrefix(3))});
  EXPECT_EQ(s.RunPending(), 2u);
  const auto calls = c.Calls();
  ASSERT_EQ(calls.size(), 3u);
  EXPECT_EQ(calls[0].what, "checkpoint");
  EXPECT_EQ(calls[1].what, "compact");
  EXPECT_EQ(calls[1].cf, ColumnFamily::kMeta);
  EXPECT_EQ(calls[2].cf, ColumnFamily::kSubkey);
  EXPECT_EQ(s.completed(), 2u);
}

TEST(CompactionScheduler, FlushAllCompactsWholeColumnFamilies) {
  FakeCompactor c;
  CompactionScheduler s(c, Opts(1000));
  s.OnCommitted({{ColumnFamily::kMeta, encoding::DbPrefix(0), encoding::DbPrefix(16), false}});
  EXPECT_EQ(s.RunPending(), 1u);
  EXPECT_EQ(c.Calls()[1].what, "compact");
  EXPECT_EQ(c.Calls()[1].cf, ColumnFamily::kMeta);
}

TEST(CompactionScheduler, CollectionDeletesTriggerAtTheThreshold) {
  FakeCompactor c;
  CompactionScheduler s(c, Opts(3));
  const std::string obj = encoding::SubkeyPrefix(0, 1, 42);
  s.OnCommitted({Prefix(ColumnFamily::kSubkey, obj), Prefix(ColumnFamily::kZScore, obj)});  // counts once
  s.OnCommitted({Prefix(ColumnFamily::kSubkey, obj)});
  EXPECT_EQ(s.pending(), 0u);
  s.OnCommitted({Prefix(ColumnFamily::kSubkey, obj)});
  EXPECT_EQ(s.pending(), 2u);  // whole subkey and zscore CFs
  EXPECT_EQ(s.RunPending(), 2u);
}

TEST(CompactionScheduler, SlotlessCollectionDeletesAreCounted) {  // their prefix is 9 bytes, not 11
  const encoding::ScopedKeyLayout slotless(encoding::KeyLayout::kSlotless);
  FakeCompactor c;
  CompactionScheduler s(c, Opts(2));
  const std::string obj = encoding::SubkeyPrefix(0, 0, 42);
  ASSERT_EQ(obj.size(), 9u);
  s.OnCommitted({Prefix(ColumnFamily::kSubkey, obj)});
  EXPECT_EQ(s.pending(), 0u);
  s.OnCommitted({Prefix(ColumnFamily::kSubkey, obj)});
  EXPECT_EQ(s.pending(), 2u);
}

RangeDelete Elements(ColumnFamily cf, std::string p, uint64_t n) {
  RangeDelete d = Prefix(cf, std::move(p));
  d.elements = n;
  return d;
}

CompactionScheduler::Options DeadOpts(uint64_t threshold, uint64_t keys, double ratio) {
  CompactionScheduler::Options o = Opts(threshold);
  o.dead_ratio = [ratio] { return ratio; };
  o.subkey_keys = [keys] { return keys; };
  return o;
}

TEST(CompactionScheduler, DeadRatioHoldsCompactionUntilEnoughIsDead) {
  FakeCompactor c;
  CompactionScheduler s(c, DeadOpts(3, 1000, 0.2));
  const std::string obj = encoding::SubkeyPrefix(0, 1, 42);
  for (int i = 0; i < 3; ++i) s.OnCommitted({Elements(ColumnFamily::kSubkey, obj, 50)});
  s.EvaluateCollectionTrigger();
  EXPECT_EQ(s.pending(), 0u);  // 150 < 200
  for (int i = 0; i < 2; ++i) s.OnCommitted({Elements(ColumnFamily::kSubkey, obj, 50)});
  s.EvaluateCollectionTrigger();
  EXPECT_EQ(s.pending(), 2u);  // 250 >= 200
}

TEST(CompactionScheduler, ZeroKeysFallsBackToCount) {  // everything still in memtables: no division, no hold
  FakeCompactor c;
  CompactionScheduler s(c, DeadOpts(3, 0, 0.2));
  const std::string obj = encoding::SubkeyPrefix(0, 1, 42);
  for (int i = 0; i < 2; ++i) s.OnCommitted({Elements(ColumnFamily::kSubkey, obj, 5)});
  s.EvaluateCollectionTrigger();
  EXPECT_EQ(s.pending(), 0u);
  s.OnCommitted({Elements(ColumnFamily::kSubkey, obj, 5)});
  s.EvaluateCollectionTrigger();
  EXPECT_EQ(s.pending(), 2u);
}

// Replicated deletes, slot import and rehoming reach the scheduler without an element count: the ratio
// can never be met for them, so the count threshold alone decides.
TEST(CompactionScheduler, UnknownElementCountsFallBackToTheCountThreshold) {
  FakeCompactor c;
  CompactionScheduler s(c, DeadOpts(3, 1000000, 0.2));
  const std::string obj = encoding::SubkeyPrefix(0, 1, 42);
  for (int i = 0; i < 3; ++i) s.OnCommitted({Prefix(ColumnFamily::kSubkey, obj)});  // elements = 0
  s.EvaluateCollectionTrigger();
  EXPECT_EQ(s.pending(), 2u);
}

TEST(CompactionScheduler, UnknownFlagResetsAfterACompactionIsQueued) {
  FakeCompactor c;
  CompactionScheduler s(c, DeadOpts(3, 1000000, 0.2));
  const std::string obj = encoding::SubkeyPrefix(0, 1, 42);
  for (int i = 0; i < 3; ++i) s.OnCommitted({Prefix(ColumnFamily::kSubkey, obj)});
  s.EvaluateCollectionTrigger();
  ASSERT_EQ(s.RunPending(), 2u);
  for (int i = 0; i < 3; ++i) s.OnCommitted({Elements(ColumnFamily::kSubkey, obj, 1)});
  s.EvaluateCollectionTrigger();
  EXPECT_EQ(s.pending(), 0u);  // known counts again: 3 << 0.2 * 1000000
}

// The ratio may defer a compaction, but not for good: range-tombstone fragments pile up meanwhile.
TEST(CompactionScheduler, RatioIsIgnoredAtTheDeferralCeiling) {
  FakeCompactor c;
  const uint64_t threshold = 3;
  CompactionScheduler s(c, DeadOpts(threshold, 1000000000, 0.2));
  const std::string obj = encoding::SubkeyPrefix(0, 1, 42);
  const uint64_t ceiling = threshold * CompactionScheduler::kMaxDeferralFactor;
  for (uint64_t i = 0; i < ceiling - 1; ++i) s.OnCommitted({Elements(ColumnFamily::kSubkey, obj, 1)});
  s.EvaluateCollectionTrigger();
  EXPECT_EQ(s.pending(), 0u);
  s.OnCommitted({Elements(ColumnFamily::kSubkey, obj, 1)});
  s.EvaluateCollectionTrigger();
  EXPECT_EQ(s.pending(), 2u);
}

TEST(CompactionScheduler, DeadRatioZeroIsTheOldBehaviour) {
  FakeCompactor c;
  CompactionScheduler s(c, DeadOpts(3, 1000, 0.0));
  const std::string obj = encoding::SubkeyPrefix(0, 1, 42);
  for (int i = 0; i < 3; ++i) s.OnCommitted({Elements(ColumnFamily::kSubkey, obj, 1)});
  EXPECT_EQ(s.pending(), 2u);  // at once, no evaluation step
}

// The commit path must not read storage stats: subkey_keys runs only in EvaluateCollectionTrigger.
TEST(CompactionScheduler, CommitsNeverReadTheKeyCount) {
  FakeCompactor c;
  std::atomic<int> reads{0};
  CompactionScheduler::Options o = DeadOpts(3, 1000, 0.2);
  o.subkey_keys = [&reads] {
    ++reads;
    return uint64_t{1000};
  };
  CompactionScheduler s(c, std::move(o));
  const std::string obj = encoding::SubkeyPrefix(0, 1, 42);
  for (int i = 0; i < 50; ++i) s.OnCommitted({Elements(ColumnFamily::kSubkey, obj, 1)});
  EXPECT_EQ(reads.load(), 0);
  EXPECT_EQ(s.pending(), 0u);
  s.EvaluateCollectionTrigger();
  EXPECT_EQ(reads.load(), 1);
  s.EvaluateCollectionTrigger();  // nothing new to decide
  EXPECT_EQ(reads.load(), 1);
}

TEST(CompactionScheduler, ChangedDeadRatioTakesEffectOnTheNextCheck) {
  FakeCompactor c;
  std::atomic<double> ratio{0.5};
  CompactionScheduler::Options o = DeadOpts(3, 1000, 0.5);
  o.dead_ratio = [&ratio] { return ratio.load(); };
  CompactionScheduler s(c, std::move(o));
  const std::string obj = encoding::SubkeyPrefix(0, 1, 42);
  for (int i = 0; i < 3; ++i) s.OnCommitted({Elements(ColumnFamily::kSubkey, obj, 100)});
  s.EvaluateCollectionTrigger();
  EXPECT_EQ(s.pending(), 0u);  // 300 < 500
  ratio = 0.2;
  s.OnCommitted({Elements(ColumnFamily::kSubkey, obj, 100)});
  s.EvaluateCollectionTrigger();
  EXPECT_EQ(s.pending(), 2u);  // 400 >= 200
}

TEST(CompactionScheduler, FlushDbIgnoresDeadRatio) {
  FakeCompactor c;
  CompactionScheduler s(c, DeadOpts(3, 1000, 0.2));
  s.OnCommitted({Prefix(ColumnFamily::kSubkey, encoding::DbPrefix(3))});
  EXPECT_EQ(s.pending(), 1u);
}

TEST(CompactionScheduler, ThresholdZeroNeverCompactsForCollections) {
  FakeCompactor c;
  CompactionScheduler s(c, Opts(0));
  for (int i = 0; i < 100; ++i)
    s.OnCommitted({Prefix(ColumnFamily::kSubkey, encoding::SubkeyPrefix(0, 1, i))});
  EXPECT_EQ(s.pending(), 0u);
}

TEST(CompactionScheduler, QueuedJobsAreDeduplicatedPerColumnFamily) {
  FakeCompactor c;
  CompactionScheduler s(c, Opts(1000));
  s.OnCommitted({Prefix(ColumnFamily::kMeta, encoding::DbPrefix(1))});
  s.OnCommitted({Prefix(ColumnFamily::kMeta, encoding::DbPrefix(1))});
  EXPECT_EQ(s.pending(), 1u);
  s.OnCommitted({{ColumnFamily::kMeta, encoding::DbPrefix(0), encoding::DbPrefix(16), false}});
  s.OnCommitted({Prefix(ColumnFamily::kMeta, encoding::DbPrefix(2))});
  EXPECT_EQ(s.pending(), 1u);  // all covered by the one queued meta compaction
  s.OnCommitted({Prefix(ColumnFamily::kExpire, encoding::DbPrefix(2))});
  EXPECT_EQ(s.pending(), 2u);
}

// Busy means another compaction (TidesDB's own, usually) holds the CF. It ends by itself, however long a
// big CF or a slow machine makes it, so it is waited out: it never uses an attempt and is never a failure
// (a give-up would drop the FLUSHDB reclaim and report a failure that did not happen).
TEST(CompactionScheduler, BusyIsWaitedOutAndNeverCountedAsAFailure) {
  FakeCompactor c;
  c.busy_left = 10;
  auto o = Opts(1000);
  o.max_attempts = 3;
  CompactionScheduler s(c, o);
  s.OnCommitted({Prefix(ColumnFamily::kMeta, encoding::DbPrefix(0))});
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(s.RunPending(), 0u) << "busy round " << i << ": requeued";
    EXPECT_EQ(s.pending(), 1u);
  }
  EXPECT_EQ(s.RunPending(), 1u);
  EXPECT_EQ(s.completed(), 1u);
  EXPECT_EQ(s.failed(), 0u);
}

// A Busy job is requeued as it was: its checkpoint already put the tombstones in sstables, and checkpointing
// again on every retry rotated a tiny memtable each time, which filled L1 with tiny flushes and stalled
// every write behind TidesDB's admission throttle (W6, 2026-11-10).
TEST(CompactionScheduler, BusyRetriesDoNotCheckpointAgain) {
  FakeCompactor c;
  c.busy_left = 3;
  CompactionScheduler s(c, Opts(1000));
  s.OnCommitted({Prefix(ColumnFamily::kMeta, encoding::DbPrefix(0))});
  for (int i = 0; i < 3; ++i) EXPECT_EQ(s.RunPending(), 0u);
  EXPECT_EQ(s.RunPending(), 1u);
  int checkpoints = 0;
  for (const Call& call : c.Calls()) checkpoints += call.what == "checkpoint";
  EXPECT_EQ(checkpoints, 1);
}

TEST(CompactionScheduler, ANewJobAfterABusyRetryStillGetsItsCheckpoint) {
  FakeCompactor c;
  c.busy_left = 1;
  CompactionScheduler s(c, Opts(1000));
  s.OnCommitted({Prefix(ColumnFamily::kMeta, encoding::DbPrefix(0))});
  EXPECT_EQ(s.RunPending(), 0u);                                         // meta busy: requeued
  s.OnCommitted({Prefix(ColumnFamily::kExpire, encoding::DbPrefix(0))});  // a new job, never checkpointed
  EXPECT_EQ(s.RunPending(), 2u);
  const auto calls = c.Calls();
  ASSERT_EQ(calls.size(), 5u);
  EXPECT_EQ(calls[2].what, "checkpoint");
}

// A fake clock the compactor advances by the time each whole-CF compaction takes.
struct FakeClock {
  std::chrono::steady_clock::time_point t{};
  std::function<std::chrono::steady_clock::time_point()> Fn() {
    return [this] { return t; };
  }
};

class TimedCompactor final : public Compactor {
 public:
  explicit TimedCompactor(FakeClock& clock) : clock_(clock) {}
  Status Checkpoint() override { return Status::Ok(); }
  Status Compact(ColumnFamily cf) override {
    if (cf == ColumnFamily::kSubkey) clock_.t += std::chrono::seconds(10);
    ++compacts[CfIndex(cf)];
    return Status::Ok();
  }
  int compacts[kColumnFamilyCount] = {};

 private:
  FakeClock& clock_;
};

// The collection-delete compaction rewrites the whole subkey CF (87 s on the medium bench) and DELs of big
// hashes reach the threshold every few seconds, so back-to-back runs took a core for good. The next one
// waits collection_compaction_spacing times the last one's length after it ended (here 4 x 10 s).
TEST(CompactionScheduler, CollectionCompactionsAreSpacedByTheirOwnLength) {
  FakeClock clock;
  TimedCompactor c(clock);
  auto o = Opts(2);
  o.now = clock.Fn();
  o.collection_compaction_spacing = 4;
  CompactionScheduler s(c, o);
  auto del = [&s](int oid) { s.OnCommitted({Prefix(ColumnFamily::kSubkey, encoding::SubkeyPrefix(0, 1, oid))}); };
  del(1);
  del(2);
  ASSERT_EQ(s.pending(), 2u);
  EXPECT_EQ(s.RunPending(), 2u);  // subkey ends at t = 10 s
  del(3);
  del(4);
  EXPECT_EQ(s.pending(), 0u) << "too soon: not before t = 50 s";
  clock.t += std::chrono::seconds(39);  // t = 49 s
  del(5);
  EXPECT_EQ(s.pending(), 0u);
  clock.t += std::chrono::seconds(1);  // t = 50 s: the deletes counted meanwhile are still owed a compaction
  del(6);
  EXPECT_EQ(s.pending(), 2u);
  EXPECT_EQ(s.RunPending(), 2u);
  EXPECT_EQ(c.compacts[CfIndex(ColumnFamily::kSubkey)], 2);
}

TEST(CompactionScheduler, FlushDbIsNotHeldBackBySpacing) {
  FakeClock clock;
  TimedCompactor c(clock);
  auto o = Opts(1);
  o.now = clock.Fn();
  CompactionScheduler s(c, o);
  s.OnCommitted({Prefix(ColumnFamily::kSubkey, encoding::SubkeyPrefix(0, 1, 1))});
  EXPECT_EQ(s.RunPending(), 2u);
  s.OnCommitted({Prefix(ColumnFamily::kSubkey, encoding::DbPrefix(0))});  // FLUSHDB 0
  EXPECT_EQ(s.pending(), 1u);
}

TEST(CompactionScheduler, SpacingZeroCompactsAtEveryThreshold) {
  FakeClock clock;
  TimedCompactor c(clock);
  auto o = Opts(1);
  o.now = clock.Fn();
  o.collection_compaction_spacing = 0;
  CompactionScheduler s(c, o);
  s.OnCommitted({Prefix(ColumnFamily::kSubkey, encoding::SubkeyPrefix(0, 1, 1))});
  EXPECT_EQ(s.RunPending(), 2u);
  s.OnCommitted({Prefix(ColumnFamily::kSubkey, encoding::SubkeyPrefix(0, 1, 2))});
  EXPECT_EQ(s.pending(), 2u);
}

// After a Busy round the worker waits busy_retry, not tick: a whole-CF compaction TidesDB started itself can
// run for a minute, and polling it every tick did nothing but take locks.
TEST(CompactionScheduler, WorkerWaitsBusyRetryAfterABusyRound) {
  FakeCompactor c;
  c.busy_left = 1;
  auto o = Opts(1000);
  o.tick = std::chrono::milliseconds(5);
  o.busy_retry = std::chrono::milliseconds(400);
  CompactionScheduler s(c, o);
  s.OnCommitted({Prefix(ColumnFamily::kMeta, encoding::DbPrefix(0))});
  s.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  int compacts = 0;
  for (const Call& call : c.Calls()) compacts += call.what == "compact";
  EXPECT_EQ(compacts, 1) << "retried before busy_retry";
  for (int i = 0; i < 400 && s.completed() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  s.Stop();
  EXPECT_EQ(s.completed(), 1u);
}

class FailingCompactor final : public Compactor {
 public:
  Status Checkpoint() override { return Status::Ok(); }
  Status Compact(ColumnFamily) override {
    ++compacts;
    return busy_first-- > 0 ? Status::Busy("compaction running") : Status::IOError("injected");
  }
  int compacts = 0;
  int busy_first = 0;
};

TEST(CompactionScheduler, AnErrorIsRetriedThenCountedAsAFailureAndBusyRoundsDoNotCount) {
  FailingCompactor c;
  c.busy_first = 4;
  auto o = Opts(1000);
  o.max_attempts = 3;
  CompactionScheduler s(c, o);
  s.OnCommitted({Prefix(ColumnFamily::kMeta, encoding::DbPrefix(0))});
  for (int i = 0; i < 4; ++i) EXPECT_EQ(s.RunPending(), 0u);  // busy: waited out
  EXPECT_EQ(s.RunPending(), 0u);                              // attempt 1: requeued
  EXPECT_EQ(s.RunPending(), 0u);                              // attempt 2: requeued
  EXPECT_EQ(s.RunPending(), 1u);                              // attempt 3: given up
  EXPECT_EQ(c.compacts, 7);
  EXPECT_EQ(s.failed(), 1u);
  EXPECT_EQ(s.pending(), 0u);
}

TEST(CompactionScheduler, WorkerThreadRunsJobs) {
  FakeCompactor c;
  auto o = Opts(1000);
  o.tick = std::chrono::milliseconds(5);
  CompactionScheduler s(c, o);
  s.Start();
  s.OnCommitted({Prefix(ColumnFamily::kMeta, encoding::DbPrefix(0))});
  for (int i = 0; i < 400 && s.completed() == 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  s.Stop();
  EXPECT_EQ(s.completed(), 1u);
}

class BlockingCompactor final : public Compactor {
 public:
  Status Checkpoint() override { return Status::Ok(); }
  Status Compact(ColumnFamily) override {
    std::unique_lock lock(mu);
    ++started;
    cv.notify_all();
    cv.wait(lock, [this] { return released; });
    return Status::Ok();
  }
  void WaitStarted() {
    std::unique_lock lock(mu);
    cv.wait(lock, [this] { return started > 0; });
  }
  void Release() {
    {
      std::lock_guard lock(mu);
      released = true;
    }
    cv.notify_all();
  }
  std::mutex mu;
  std::condition_variable cv;
  int started = 0;
  bool released = false;
};

TEST(CompactionScheduler, StopDropsJobsThatHaveNotStarted) {
  BlockingCompactor c;
  auto o = Opts(1000);
  o.tick = std::chrono::milliseconds(5);
  CompactionScheduler s(c, o);
  s.OnCommitted({Prefix(ColumnFamily::kMeta, encoding::DbPrefix(1)),
                 Prefix(ColumnFamily::kExpire, encoding::DbPrefix(1))});
  ASSERT_EQ(s.pending(), 2u);
  s.Start();
  c.WaitStarted();  // first job is inside Compact
  std::thread stopper([&] { s.Stop(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let Stop raise the flag
  c.Release();
  stopper.join();
  EXPECT_EQ(s.completed(), 1u);
  EXPECT_EQ(s.failed(), 0u);
  EXPECT_EQ(c.started, 1);  // the second job never ran
}

}  // namespace
}  // namespace tideskv
