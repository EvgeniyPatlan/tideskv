#include "migrate/slot_migrator.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <string>

#include "cluster/cluster_errors.h"
#include "repl/backlog_trimmer.h"
#include "support/migration_harness.h"
#include "types/errors.h"

namespace tideskv::test {
namespace {

using namespace std::chrono_literals;
using cluster::MigrateState;

// Review Focus #1: an acknowledged write at every phase before the fence (in the snapshot, in the catch-up,
// the instant before the barrier) is on the target after success; a write during the fence answers TRYAGAIN
// and is applied nowhere.
TEST(SlotMigratorTest, AWriteJustBeforeTheFenceReachesTheTarget) {
  MigrationPair p;
  ASSERT_TRUE(
      p.source.services.config_.Set("migrate-sequence-gap", "1", true).ok());  // force a catch-up round
  ASSERT_EQ(p.source.Run({"SET", "b", "snapshot"}), kOk);
  ASSERT_EQ(p.source.Run({"HSET", "{b}h", "f", "v"}), Int(1));
  SecondClient writer(p.source);
  std::string in_catch_up, before_fence, during_fence, read_during;
  migrate::SlotMigrator::Hooks hooks;
  hooks.after_snapshot = [&] {
    in_catch_up = writer.Run({"SET", "{b}catchup", "acked"});
    writer.Run({"HDEL", "{b}h", "f"});  // a collection emptied after the snapshot
  };
  hooks.before_fence = [&] { before_fence = writer.Run({"SET", "{b}late", "acked"}); };
  hooks.after_fence = [&] {
    during_fence = writer.Run({"SET", "{b}fenced", "x"});
    read_during = writer.Run({"GET", "b"});
  };
  p.source_migration().migrator().SetHooks(hooks);
  ASSERT_TRUE(RunJob(p, {3300, 3300}).ok());
  EXPECT_EQ(in_catch_up, kOk);
  EXPECT_EQ(before_fence, kOk);
  EXPECT_EQ(during_fence, Err(kErrTryAgain));  // Route's fast path: the fence is published before the hook
  EXPECT_EQ(read_during, Bulk("snapshot"));    // reads are never fenced
  EXPECT_EQ(p.target.Run({"GET", "b"}), Bulk("snapshot"));
  EXPECT_EQ(p.target.Run({"GET", "{b}catchup"}), Bulk("acked"));
  EXPECT_EQ(p.target.Run({"GET", "{b}late"}), Bulk("acked"));
  EXPECT_EQ(p.target.Run({"EXISTS", "{b}h"}), Int(0));
  EXPECT_EQ(p.target.Run({"GET", "{b}fenced"}), kNil);
  EXPECT_EQ(p.source.Cf(ColumnFamily::kMeta).count(encoding::MetaKeyFor(0, "{b}fenced")), 0u);
  EXPECT_EQ(p.source.Run({"GET", "b"}), Err("MOVED 3300 127.0.0.1:7001"));
  const auto s = p.source_cluster.manager->States();
  EXPECT_EQ(s->job.value().state, MigrateState::kSuccess);
  ASSERT_EQ(s->migrated.size(), 1u);
  EXPECT_TRUE(p.source.services.engine_.SlotFenced(3300));
  EXPECT_EQ(p.target.Run({"DEBUG", "DIGEST-SLOTS", "3300", "TTL"}),
            p.source.Run({"DEBUG", "DIGEST-SLOTS", "3300", "TTL"}));
}

TEST(SlotMigratorTest, StageErrorsFailTheJobAndLowerTheFence) {  // KM §2.3
  MigrationPair p;
  p.source_cluster.transport =
      [](const std::string&, uint16_t, std::chrono::milliseconds,
         const std::atomic<bool>*) -> Result<std::unique_ptr<migrate::MigrationTransport>> {
    return Status::IOError("connect: Connection refused");
  };
  EXPECT_EQ(RunJob(p, {3300, 3300}).message(),
            std::string(cluster::kErrStageConnect) + "connect: Connection refused");
  EXPECT_EQ(p.source_cluster.manager->States()->job.value().state, MigrateState::kFail);
  EXPECT_FALSE(p.source.services.engine_.SlotFenced(3300));
  MigrationPair q;  // AUTH with this node's own requirepass (KM §5): the target has another one
  ASSERT_TRUE(q.source.services.config_.Set("requirepass", "pass", true).ok());
  ASSERT_TRUE(q.target.services.config_.Set("requirepass", "password", true).ok());
  EXPECT_TRUE(RunJob(q, {3300, 3300}).message().starts_with(cluster::kErrStageAuth));
  ASSERT_TRUE(q.source.services.config_.Set("requirepass", "password", true).ok());
  EXPECT_TRUE(RunJob(q, {3300, 3300}).ok());
  EXPECT_EQ(RunJob(q, {12182, 12182}).message(),  // the target owns 12182
            std::string(cluster::kErrStageImportStatus) + std::string(cluster::kErrImportOwnSlot));
}

TEST(SlotMigratorTest, ACancelFailsTheJobAndTellsTheTarget) {  // D10
  MigrationPair p;
  ASSERT_EQ(p.source.Run({"SET", "b", "1"}), kOk);
  migrate::SlotMigrator::Hooks hooks;
  hooks.after_snapshot = [&] { p.source_migration().migrator().Cancel(); };
  p.source_migration().migrator().SetHooks(hooks);
  EXPECT_EQ(RunJob(p, {3300, 3300}).message(), cluster::kErrMigrationCanceled);
  EXPECT_EQ(p.target_cluster.manager->States()->import.value().state, cluster::ImportState::kError);
  EXPECT_EQ(p.target.Run({"CLUSTER", "COUNTKEYSINSLOT", "3300"}), Int(0));
  EXPECT_FALSE(p.source.services.engine_.SlotFenced(3300));
  EXPECT_EQ(p.source.Run({"GET", "b"}), Bulk("1"));  // the source serves again (D1)
}

TEST(SlotMigratorTest, AFlushSeenInTheCatchUpCancelsTheJob) {  // D2, D10
  MigrationPair p;
  ASSERT_TRUE(p.source.services.config_.Set("migrate-sequence-gap", "1", true).ok());
  ASSERT_EQ(p.source.Run({"SET", "b", "1"}), kOk);
  SecondClient writer(p.source);
  migrate::SlotMigrator::Hooks hooks;
  hooks.after_snapshot = [&] { writer.Run({"FLUSHALL"}); };
  p.source_migration().migrator().SetHooks(hooks);
  EXPECT_EQ(RunJob(p, {3300, 3300}).message(), cluster::kErrMigrationCanceled);
  EXPECT_EQ(p.target.Run({"CLUSTER", "COUNTKEYSINSLOT", "3300"}), Int(0));
}

TEST(SlotMigratorTest, TheBacklogPinFollowsTheJob) {  // D19
  MigrationPair p;
  ASSERT_EQ(p.source.Run({"SET", "b", "1"}), kOk);
  migrate::SlotMigrator& migrator = p.source_migration().migrator();
  EXPECT_EQ(migrator.oldest_needed(), repl::kNoReplicaNeeds);
  uint64_t pinned = 0;
  uint64_t last = 0;
  migrate::SlotMigrator::Hooks hooks;
  hooks.after_snapshot = [&] {
    pinned = p.source_migration().OldestNeeded();
    last = p.source.services.repl_log_.last();
  };
  migrator.SetHooks(hooks);
  ASSERT_TRUE(RunJob(p, {3300, 3300}).ok());
  EXPECT_EQ(pinned, last + 1);
  EXPECT_EQ(migrator.oldest_needed(), repl::kNoReplicaNeeds);
}

TEST(SlotMigratorTest, ABacklogTrimmedPastTheJobFailsIt) {  // D19, D24.4
  MigrationPair p;
  ASSERT_TRUE(p.source.services.config_.Set("migrate-sequence-gap", "1", true).ok());
  ASSERT_EQ(p.source.Run({"SET", "b", "1"}), kOk);
  SecondClient writer(p.source);
  migrate::SlotMigrator::Hooks hooks;
  hooks.after_snapshot = [&] {
    writer.Run({"SET", "{b}x", "1"});
    writer.Run({"SET", "{b}y", "1"});
    ASSERT_TRUE(p.source.services.repl_log_.Trim(p.source.services.repl_log_.last() - 1).ok());
  };
  p.source_migration().migrator().SetHooks(hooks);
  const Status st = RunJob(p, {3300, 3300});
  EXPECT_TRUE(st.message().starts_with(std::string(cluster::kErrStageIncremental) + "replog entry "))
      << st.ToString();
}

TEST(SlotMigratorTest, TheDelayKnobHoldsTheSnapshotPhaseAndTheFence) {  // D25
  MigrationPair p;
  ASSERT_TRUE(p.source.services.config_.Set("migrate-delay-ms", "200", true).ok());
  const auto start = std::chrono::steady_clock::now();
  ASSERT_TRUE(RunJob(p, {3300, 3300}).ok());
  EXPECT_GE(std::chrono::steady_clock::now() - start, 400ms);
}

TEST(SlotMigratorTest, OneJobAtATimeAndStopEndsAJobAtOnce) {
  MigrationPair p;
  ASSERT_TRUE(p.source.services.config_.Set("migrate-delay-ms", "60000", true).ok());
  auto done = std::make_shared<std::promise<Status>>();
  ASSERT_TRUE(
      StartJob(p.source_migration(), {3300, 3300}, [done](const Status& s) { done->set_value(s); }).ok());
  EXPECT_EQ(
      p.source_migration().migrator().Start({{5061, 5061}, kNodeB, "127.0.0.1", 7001}, nullptr).message(),
      cluster::kErrAlreadyMigrating);
  const auto start = std::chrono::steady_clock::now();
  p.source_migration().migrator().Stop();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
  EXPECT_EQ(done->get_future().get().message(), cluster::kErrMigrationCanceled);
  EXPECT_FALSE(p.source_migration().migrator().running());
}

}  // namespace
}  // namespace tideskv::test
