#include "server/compaction_scheduler.h"

#include <gtest/gtest.h>

#include <chrono>
#include <mutex>
#include <thread>

#include "encoding/key_codec.h"
#include "encoding/object_codec.h"

namespace tideskv {
namespace {

struct Call {
  std::string what;  // "checkpoint" or "compact"
  ColumnFamily cf = ColumnFamily::kMeta;
  std::string lo, hi;
};

class FakeCompactor final : public Compactor {
 public:
  Status Checkpoint() override {
    std::lock_guard lock(mu);
    calls.push_back({"checkpoint", ColumnFamily::kMeta, {}, {}});
    return Status::Ok();
  }
  Status CompactRange(ColumnFamily cf, std::string_view lo, std::string_view hi) override {
    std::lock_guard lock(mu);
    calls.push_back({"compact", cf, std::string(lo), std::string(hi)});
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

TEST(CompactionScheduler, FlushDbCompactsThatDbsRangeAfterACheckpoint) {
  FakeCompactor c;
  CompactionScheduler s(c, Opts(1000));
  s.OnCommitted({Prefix(ColumnFamily::kMeta, encoding::DbPrefix(3)),
                 Prefix(ColumnFamily::kSubkey, encoding::DbPrefix(3))});
  EXPECT_EQ(s.RunPending(), 2u);
  const auto calls = c.Calls();
  ASSERT_EQ(calls.size(), 3u);
  EXPECT_EQ(calls[0].what, "checkpoint");
  EXPECT_EQ(calls[1].lo, encoding::DbPrefix(3));
  EXPECT_EQ(calls[1].hi, encoding::DbPrefix(4));
  EXPECT_EQ(s.completed(), 2u);
}

TEST(CompactionScheduler, FlushAllCompactsWholeColumnFamilies) {
  FakeCompactor c;
  CompactionScheduler s(c, Opts(1000));
  s.OnCommitted({{ColumnFamily::kMeta, encoding::DbPrefix(0), encoding::DbPrefix(16), false}});
  EXPECT_EQ(s.RunPending(), 1u);
  EXPECT_EQ(c.Calls()[1].lo, "");
  EXPECT_EQ(c.Calls()[1].hi, "");
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

TEST(CompactionScheduler, DuplicatesAreDroppedAndWholeCfJobsSubsumeRanges) {
  FakeCompactor c;
  CompactionScheduler s(c, Opts(1000));
  s.OnCommitted({Prefix(ColumnFamily::kMeta, encoding::DbPrefix(1))});
  s.OnCommitted({Prefix(ColumnFamily::kMeta, encoding::DbPrefix(1))});
  EXPECT_EQ(s.pending(), 1u);
  s.OnCommitted({{ColumnFamily::kMeta, encoding::DbPrefix(0), encoding::DbPrefix(16), false}});
  EXPECT_EQ(s.pending(), 1u);  // the whole-CF job replaced the range job
  s.OnCommitted({Prefix(ColumnFamily::kMeta, encoding::DbPrefix(2))});
  EXPECT_EQ(s.pending(), 1u);  // already covered
}

TEST(CompactionScheduler, BusyIsRetriedThenCountedAsAFailure) {
  FakeCompactor c;
  c.busy_left = 100;
  auto o = Opts(1000);
  o.max_attempts = 3;
  CompactionScheduler s(c, o);
  s.OnCommitted({Prefix(ColumnFamily::kMeta, encoding::DbPrefix(0))});
  EXPECT_EQ(s.RunPending(), 0u);  // attempt 1: requeued
  EXPECT_EQ(s.RunPending(), 0u);  // attempt 2: requeued
  EXPECT_EQ(s.RunPending(), 1u);  // attempt 3: given up
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

}  // namespace
}  // namespace tideskv
