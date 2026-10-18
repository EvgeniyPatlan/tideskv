#include "server/compaction_scheduler.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "encoding/key_codec.h"
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
