#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "cluster/cluster_errors.h"
#include "support/controller_info.h"
#include "support/migration_harness.h"

namespace tideskv::test {
namespace {

using namespace std::chrono_literals;

const std::string kMoved = kNodeA + " 127.0.0.1 7000 master - 0-3299 3301-8191\n" + kNodeB +
                           " 127.0.0.1 7001 master - 3300 8192-16383\n" + kNodeC + " 127.0.0.1 7002 slave " +
                           kNodeA + "\n";

std::string InfoOf(CommandHarness& h) {
  const std::string reply = h.Run({"CLUSTER", "INFO"});
  const size_t body = reply.find("\r\n") + 2;
  return reply.substr(body, reply.size() - body - 2);
}

TEST(MigrationTransitions, ASlotOnlyReassignmentMidJobCancelsItAndKeepsTheData) {  // D7, D10
  MigrationPair p;
  ASSERT_EQ(p.source.Run({"SET", "b", "1"}), kOk);
  ASSERT_TRUE(p.source.services.config_.Set("migrate-delay-ms", "60000", true).ok());
  auto done = std::make_shared<std::promise<Status>>();
  ASSERT_TRUE(
      StartJob(p.source_migration(), {3300, 3300}, [done](const Status& s) { done->set_value(s); }).ok());
  p.source_cluster.Push(kMoved, 2);  // slot_only: the range leaves without a migration
  auto outcome = done->get_future();
  ASSERT_EQ(outcome.wait_for(10s), std::future_status::ready);
  EXPECT_EQ(outcome.get().message(), cluster::kErrMigrationCanceled);
  ASSERT_TRUE(p.source_migration().AwaitIdle(5s));
  EXPECT_FALSE(p.source.services.engine_.SlotFenced(3300));
  EXPECT_EQ(p.source.Run({"CLUSTER", "COUNTKEYSINSLOT", "3300"}), Int(1)) << "never migrated: kept (Kvrocks)";
}

TEST(MigrationTransitions, AnUnrelatedPushChangesNothing) {  // D7: never cancel on the controller's v+1
  MigrationPair p;
  ASSERT_TRUE(p.source.services.config_.Set("migrate-delay-ms", "300", true).ok());
  auto done = std::make_shared<std::promise<Status>>();
  ASSERT_TRUE(
      StartJob(p.source_migration(), {3300, 3300}, [done](const Status& s) { done->set_value(s); }).ok());
  p.source_cluster.Push(kThreeNodes, 2);
  p.target_cluster.Push(kThreeNodes, 2);
  EXPECT_TRUE(done->get_future().get().ok());
  EXPECT_EQ(p.target_cluster.manager->States()->imported.size(), 1u);
}

TEST(MigrationTransitions, ADemotionStopsTheJobAndClearsTheStates) {  // D7, gocase TestSlotMigrateThreeNodes
  MigrationPair p;
  ASSERT_TRUE(p.source.services.config_.Set("migrate-delay-ms", "60000", true).ok());
  auto done = std::make_shared<std::promise<Status>>();
  ASSERT_TRUE(
      StartJob(p.source_migration(), {3300, 3300}, [done](const Status& s) { done->set_value(s); }).ok());
  for (int i = 0; i < 5000 && !p.target_cluster.manager->States()->import; ++i) {
    std::this_thread::sleep_for(
        1ms);  // IMPORT <R> 0 reached the target: the cancel must tell it (IMPORT <R> 2)
  }
  ASSERT_TRUE(p.target_cluster.manager->States()->import.has_value());
  const std::string swapped = kNodeA + " 127.0.0.1 7000 slave " + kNodeC + "\n" + kNodeB +
                              " 127.0.0.1 7001 master - 8192-16383\n" + kNodeC +
                              " 127.0.0.1 7002 master - 0-8191\n";
  p.source_cluster.Push(swapped, 2);
  EXPECT_EQ(p.source.services.replication_.role(), ReplRole::kReplica);
  auto outcome = done->get_future();
  ASSERT_EQ(outcome.wait_for(10s), std::future_status::ready);
  EXPECT_EQ(outcome.get().message(), cluster::kErrMigrationCanceled);
  ASSERT_TRUE(p.source_migration().AwaitIdle(5s));
  EXPECT_NE(InfoOf(p.target).find("import_state: error\r\n"), std::string::npos);
  EXPECT_FALSE(p.source.services.engine_.SlotFenced(3300));
}

TEST(MigrationTransitions,
     TheSuccessBlockSurvivesEveryPushUnderTheControllersParser) {  // D15, Review Focus #5
  MigrationPair p;
  ASSERT_TRUE(RunJob(p, {3300, 3300}).ok());
  for (int64_t v : {2, 3}) {
    p.source_cluster.Push(v == 2 ? kThreeNodes : kMoved, v);
    ASSERT_TRUE(p.source_migration().AwaitIdle(5s));
    const ControllerClusterInfo c = ParseLikeController(InfoOf(p.source));
    EXPECT_TRUE(c.ok);
    EXPECT_EQ(c.epoch, v);
    EXPECT_EQ(c.migrating_slot, "3300");
    EXPECT_EQ(c.migrating_state, "success");
  }
}

// Controller ruling S2(a), a regression guard of the ordering: the push moves R away while the worker is
// busy (here: held), so its transition is still queued when IMPORT <R> 0 lands (the importer's worker wait,
// shortened here, times out) and releases the migrated entry. The transition must then not clean R up: it
// holds the import's (non-empty) data.
TEST(MigrationTransitions, AnImportThatLandsBeforeTheTransitionKeepsItsData) {
  MigrationPair p(100ms);
  ASSERT_EQ(p.source.Run({"SET", "b", "1"}), kOk);
  ASSERT_TRUE(RunJob(p, {3300, 3300}).ok());
  ASSERT_EQ(p.target.Run({"SET", "b", "back"}), kOk);  // imported on B: served there
  migrate::MigrationService& m = p.source_migration();
  auto release = std::make_shared<std::atomic<bool>>(false);
  m.Post([release, &m] {  // a long task: the worker is busy
    while (!release->load() && !m.stopping()) std::this_thread::sleep_for(1ms);
  });
  p.source_cluster.Push(kMoved, 2);  // the transition is queued behind it
  SecondClient link(p.source);
  ASSERT_EQ(link.Run({"CLUSTER", "IMPORT", "3300", "0"}), kOk);  // after its bounded wait for the worker
  EXPECT_TRUE(p.source_cluster.manager->States()->migrated.empty()) << "released by the import (D14)";
  for (const std::string& chunk : RangeChunks(p.target, {3300, 3300})) {
    ASSERT_EQ(link.Run({"CLUSTER", "IMPORTDATA", chunk}), kOk);
  }
  release->store(true);
  ASSERT_TRUE(m.AwaitIdle(5s));  // the transition ran, against the import in start
  EXPECT_TRUE(p.source_cluster.manager->States()->cleanup.empty());
  EXPECT_EQ(p.source.Run({"CLUSTER", "COUNTKEYSINSLOT", "3300"}), Int(1));
  ASSERT_EQ(link.Run({"CLUSTER", "IMPORT", "3300", "1"}), kOk);
  ASSERT_TRUE(m.AwaitIdle(5s));
  EXPECT_EQ(p.source.Run({"CLUSTER", "COUNTKEYSINSLOT", "3300"}), Int(1));
  EXPECT_EQ(p.source.Run({"GET", "b"}), Bulk("back")) << "imported: served before a push grants it";
}

}  // namespace
}  // namespace tideskv::test
