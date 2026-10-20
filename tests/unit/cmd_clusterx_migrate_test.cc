#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cluster/cluster_errors.h"
#include "support/controller_info.h"
#include "support/key_layout_skip.h"
#include "support/migration_harness.h"
#include "types/errors.h"

namespace tideskv::test {
namespace {

using namespace std::chrono_literals;

std::string InfoOf(CommandHarness& h) {
  const std::string reply = h.Run({"CLUSTER", "INFO"});
  const size_t body = reply.find("\r\n") + 2;
  return reply.substr(body, reply.size() - body - 2);
}

// The replies a parked call received on the harness's link, waiting up to 10 s for the first.
std::vector<std::string> AwaitUnblocked(CommandHarness& h) {
  for (int i = 0; i < 1000; ++i) {
    std::vector<std::string> r = h.TakeUnblocked();
    if (!r.empty()) return r;
    std::this_thread::sleep_for(10ms);
  }
  return {};
}

bool WaitState(MigrationPair& p, const char* state) {
  for (int i = 0; i < 1000; ++i) {
    if (InfoOf(p.source).find(std::string("migrating_state: ") + state + "\r\n") != std::string::npos)
      return true;
    std::this_thread::sleep_for(10ms);
  }
  return false;
}

// The job runs on the migrator thread: its IMPORT start reaches the target some time after +OK.
bool WaitImportState(CommandHarness& h, const char* state) {
  for (int i = 0; i < 1000; ++i) {
    if (InfoOf(h).find(std::string("import_state: ") + state + "\r\n") != std::string::npos) return true;
    std::this_thread::sleep_for(10ms);
  }
  return false;
}

TEST(ClusterxMigrate, ExecuteErrorsComeInKvrocksOrder) {  // cluster.cc:309-346, slot_migrate.cc:66-80
  TIDESKV_SKIP_IF_SLOTLESS();
  MigrationPair p;
  CommandHarness& a = p.source;
  EXPECT_EQ(a.Run({"CLUSTERX", "MIGRATE", "3300", std::string(40, 'z')}), Err(cluster::kErrNoDestination));
  EXPECT_EQ(a.Run({"CLUSTERX", "MIGRATE", "12182", kNodeB}), Err(cluster::kErrNotMySlot));
  EXPECT_EQ(a.Run({"CLUSTERX", "MIGRATE", "3300", kNodeC}), Err(cluster::kErrMigrateToSlave));
  EXPECT_EQ(a.Run({"CLUSTERX", "MIGRATE", "3300", kNodeA}), Err(cluster::kErrMigrateToMyself));
  EXPECT_EQ(a.Run({"CLUSTERX", "MIGRATE", "0-5 10-20", kNodeB}), Err(cluster::kErrMultipleRanges));  // D12
  CommandHarness c;  // node C, A's replica: it owns nothing, so "doesn't belong" comes first (KM §2.2)
  ClusterHarness cc(c, 7002);
  cc.Push(kThreeNodes, 1);
  EXPECT_EQ(c.Run({"CLUSTERX", "MIGRATE", "3300", kNodeB}), Err(cluster::kErrNotMySlot));
  ASSERT_EQ(a.Run({"CLUSTERX", "MIGRATE", "112-118", kNodeB}), kOk);
  ASSERT_TRUE(WaitState(p, "success"));
  // the span of the migrated ranges (gocase "does not immediately update the topology")
  EXPECT_EQ(a.Run({"CLUSTERX", "MIGRATE", "110-112", kNodeB}), Err(cluster::kErrSlotMigrated));
  EXPECT_EQ(a.Run({"CLUSTERX", "MIGRATE", "118-120", kNodeB}), Err(cluster::kErrSlotMigrated));
  ASSERT_TRUE(a.services.config_.Set("migrate-delay-ms", "60000", true).ok());
  ASSERT_EQ(a.Run({"CLUSTERX", "MIGRATE", "200", kNodeB}), kOk);
  EXPECT_EQ(a.Run({"CLUSTERX", "MIGRATE", "300", kNodeB}), Err(cluster::kErrAlreadyMigrating));
  p.source_migration().migrator().Stop();
}

TEST(ClusterxMigrate, TheStartBlockIsPublishedBeforeOk) {  // D12, Review Focus #5 "no gap"
  TIDESKV_SKIP_IF_SLOTLESS();
  MigrationPair p;
  ASSERT_TRUE(p.source.services.config_.Set("migrate-delay-ms", "60000", true).ok());
  ASSERT_EQ(p.source.Run({"CLUSTERX", "MIGRATE", "3300", kNodeB}), kOk);
  const ControllerClusterInfo c = ParseLikeController(InfoOf(p.source));  // the very next command
  EXPECT_TRUE(c.ok);
  EXPECT_EQ(c.migrating_slot, "3300");
  EXPECT_EQ(c.migrating_state, "start");
  p.source_migration().migrator().Stop();
}

TEST(ClusterxMigrate, SyncRepliesOkAfterSuccessAndTheStageErrorOnFailure) {  // KM §2.3
  TIDESKV_SKIP_IF_SLOTLESS();
  MigrationPair p;
  ASSERT_EQ(p.source.Run({"SET", "b", "1"}), kOk);
  const std::string reply = p.source.Run({"CLUSTERX", "MIGRATE", "3300", kNodeB, "SYNC"});
  if (reply.empty()) {  // parked: the job ends on the migrator thread
    EXPECT_EQ(AwaitUnblocked(p.source), (std::vector<std::string>{kOk}));
  } else {
    EXPECT_EQ(reply, kOk);  // ended before the connection parked: the rendezvous replied at once
  }
  EXPECT_NE(InfoOf(p.source).find("migrating_state: success\r\n"), std::string::npos);
  EXPECT_EQ(p.source.Run({"GET", "b"}), Err("MOVED 3300 127.0.0.1:7001"));
  p.source_cluster.transport =
      [](const std::string&, uint16_t, std::chrono::milliseconds,
         const std::atomic<bool>*) -> Result<std::unique_ptr<migrate::MigrationTransport>> {
    return Status::IOError("refused");
  };
  const std::string failed = p.source.Run({"CLUSTERX", "MIGRATE", "5061", kNodeB, "SYNC", "0"});
  const std::string expected = Err(std::string(cluster::kErrStageConnect) + "refused");
  if (failed.empty()) {
    EXPECT_EQ(AwaitUnblocked(p.source), (std::vector<std::string>{expected}));
  } else {
    EXPECT_EQ(failed, expected);
  }
}

TEST(ClusterxMigrate, SyncTimesOutWithNilAndTheJobContinues) {  // KM §2.3
  TIDESKV_SKIP_IF_SLOTLESS();
  MigrationPair p;
  ASSERT_TRUE(p.source.services.config_.Set("migrate-delay-ms", "1500", true).ok());
  EXPECT_EQ(p.source.Run({"CLUSTERX", "MIGRATE", "3300", kNodeB, "SYNC", "1"}), "");
  ASSERT_NE(p.source.session.blocked, nullptr);
  ASSERT_TRUE(p.source.session.blocked->deadline().has_value());  // 1 s
  EXPECT_EQ(p.source.session.blocked->TimeoutReply(), kNil);      // what the timer sends at the deadline
  ASSERT_TRUE(p.source.session.blocked->Cancel());                // the connection's timer fired
  EXPECT_TRUE(WaitState(p, "success")) << "the job continues after a SYNC timeout";
}

TEST(ClusterxFlushSlots, ErasesTheRangesAndIsRefusedDuringAJob) {  // D13, gocase TestClusterFlushSlots
  TIDESKV_SKIP_IF_SLOTLESS();
  MigrationPair p;
  for (const char* k : {"b", "bar", "c", "k2"})
    ASSERT_EQ(p.source.Run({"SET", k, "1"}), kOk);  // 3300 5061 7365 449
  EXPECT_EQ(p.source.Run({"CLUSTERX", "FLUSHSLOTS", "3300 449-500"}), kOk);
  EXPECT_EQ(p.source.Run({"GET", "b"}), kNil);
  EXPECT_EQ(p.source.Run({"GET", "k2"}), kNil);
  EXPECT_EQ(p.source.Run({"GET", "bar"}), Bulk("1"));
  EXPECT_EQ(p.source.Run({"DBSIZE"}), Int(2));
  ASSERT_TRUE(p.source.services.config_.Set("migrate-delay-ms", "60000", true).ok());
  ASSERT_EQ(p.source.Run({"CLUSTERX", "MIGRATE", "5061", kNodeB}), kOk);
  EXPECT_EQ(p.source.Run({"CLUSTERX", "FLUSHSLOTS", "7365"}), Err(cluster::kErrFlushWhileMigrating));
  p.source_migration().migrator().Stop();
  CommandHarness fresh;  // works on an uninitialised cluster (Kvrocks)
  ClusterHarness f(fresh);
  EXPECT_EQ(fresh.Run({"CLUSTERX", "FLUSHSLOTS", "0-16383"}), kOk);
  fresh.services.replication_.set_role(ReplRole::kReplica);
  EXPECT_EQ(fresh.Run({"CLUSTERX", "FLUSHSLOTS", "0"}), Err(kErrReadOnlyReplica));  // D13: a write
}

TEST(FlushDuringMigration, AFlushCancelsARunningJobAndFailsARunningImport) {  // D10, Writer's notes 1 and 7
  TIDESKV_SKIP_IF_SLOTLESS();
  MigrationPair p;
  ASSERT_TRUE(p.source.services.config_.Set("migrate-delay-ms", "60000", true).ok());
  ASSERT_EQ(p.source.Run({"SET", "b", "1"}), kOk);
  ASSERT_EQ(p.source.Run({"CLUSTERX", "MIGRATE", "3300", kNodeB}), kOk);
  ASSERT_TRUE(WaitImportState(p.target, "start"));  // the job holds its fence for migrate-delay-ms
  EXPECT_EQ(p.source.Run({"FLUSHALL"}), kOk);
  ASSERT_TRUE(WaitState(p, "fail"));
  EXPECT_NE(InfoOf(p.target).find("import_state: error\r\n"), std::string::npos);
  SecondClient link(p.target);  // a flush on a target fails its import
  ASSERT_EQ(link.Run({"CLUSTER", "IMPORT", "3300", "0"}), kOk);
  EXPECT_EQ(p.target.Run({"FLUSHDB"}), kOk);
  ASSERT_TRUE(p.target_migration().AwaitIdle(5s));
  EXPECT_EQ(link.Run({"CLUSTER", "IMPORTDATA", "x"}), Err(cluster::kErrImportNotRunning));
}

TEST(FlushDuringMigration, AFlushOfAMigratedFencedSlotRuns) {  // gocase "Migrate empty slot", Writer's note 1
  TIDESKV_SKIP_IF_SLOTLESS();
  MigrationPair p;
  ASSERT_EQ(p.source.Run({"SET", "b", "1"}), kOk);
  ASSERT_EQ(p.source.Run({"CLUSTERX", "MIGRATE", "3300", kNodeB}), kOk);
  ASSERT_TRUE(WaitState(p, "success"));
  ASSERT_TRUE(p.source.services.engine_.SlotFenced(3300));
  EXPECT_EQ(p.source.Run({"FLUSHDB"}), kOk);
  EXPECT_EQ(p.source.Run({"DBSIZE"}), Int(0));
  EXPECT_EQ(p.target.Run({"GET", "b"}), Bulk("1")) << "the target's imported copy is untouched";
}

// Task 10 review (controller ruling): a FLUSH during the fence commits after fence_seq, so the final delta
// never carries it; it must fail the job, lift the fence and leave the target in error.
TEST(FlushDuringMigration, AFlushWhileTheRangeIsFencedFailsTheJob) {
  TIDESKV_SKIP_IF_SLOTLESS();
  MigrationPair p;
  ASSERT_EQ(p.source.Run({"SET", "b", "1"}), kOk);
  SecondClient flusher(p.source);
  std::string flushed;
  migrate::SlotMigrator::Hooks hooks;
  hooks.after_fence = [&] { flushed = flusher.Run({"FLUSHALL"}); };
  p.source_migration().migrator().SetHooks(std::move(hooks));
  const Status outcome = RunJob(p, {3300, 3300});
  EXPECT_EQ(flushed, kOk) << "FLUSH stays unfenced (Writer's note 1)";
  EXPECT_EQ(outcome.message(), cluster::kErrMigrationCanceled);
  EXPECT_NE(InfoOf(p.source).find("migrating_state: fail\r\n"), std::string::npos);
  EXPECT_FALSE(p.source.services.engine_.SlotFenced(3300)) << "the fence is lifted";
  EXPECT_NE(InfoOf(p.target).find("import_state: error\r\n"), std::string::npos);
  EXPECT_EQ(p.target.Run({"DBSIZE"}), Int(0)) << "the target erased the half import";
}

}  // namespace
}  // namespace tideskv::test
