#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

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

TEST(MigrationTransitions, TheCleanupWaitsWhileClientsArePaused) {  // D9, M7a D21: sequence holds still
  MigrationPair p;
  ASSERT_EQ(p.source.Run({"SET", "b", "1"}), kOk);
  ASSERT_TRUE(RunJob(p, {3300, 3300}).ok());
  ASSERT_EQ(p.source.Run({"CLIENT", "PAUSE", "60000", "WRITE"}), kOk);
  const uint64_t seq = p.source.services.repl_log_.last();
  p.source_cluster.Push(kMoved, 2);  // sys/cluster_cfg is node-local: not a replicated write
  EXPECT_FALSE(p.source_migration().AwaitIdle(300ms));
  EXPECT_EQ(p.source.services.repl_log_.last(), seq) << "no replicated write while paused";
  EXPECT_EQ(p.source.Run({"CLUSTER", "COUNTKEYSINSLOT", "3300"}), Int(1));
  ASSERT_EQ(p.source.Run({"CLIENT", "UNPAUSE"}), kOk);
  ASSERT_TRUE(p.source_migration().AwaitIdle(5s));
  EXPECT_EQ(p.source.Run({"CLUSTER", "COUNTKEYSINSLOT", "3300"}), Int(0));
}

TEST(MigrationTransitions, TheSuccessRecordWaitsWhileClientsArePaused) {  // D9, ruling S5
  MigrationPair p;
  ASSERT_EQ(p.source.Run({"SET", "b", "1"}), kOk);
  std::atomic<uint64_t> seq{0};
  migrate::SlotMigrator::Hooks hooks;
  hooks.after_fence = [&p, &seq] {  // the fence is up: only the final delta and IMPORT <R> 1 remain
    p.source.services.pause_.Pause(ClientPause::Clock::now() + 60s, ClientPause::Mode::kWrite);
    seq.store(p.source.services.repl_log_.last());
  };
  p.source_migration().migrator().SetHooks(std::move(hooks));
  auto done = std::make_shared<std::promise<Status>>();
  ASSERT_TRUE(
      StartJob(p.source_migration(), {3300, 3300}, [done](const Status& s) { done->set_value(s); }).ok());
  const auto import_state = [&p] {
    const std::optional<cluster::ImportJob> import = p.target_cluster.manager->States()->import;
    return import.has_value() ? std::optional(import->state) : std::nullopt;
  };
  const auto job_state = [&p] {
    const std::optional<cluster::SourceJob> job = p.source_cluster.manager->States()->job;
    return job.has_value() ? std::optional(job->state) : std::nullopt;
  };
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (import_state() != cluster::ImportState::kSuccess && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_EQ(import_state(), cluster::ImportState::kSuccess);  // the target acked
  auto outcome = done->get_future();
  EXPECT_EQ(outcome.wait_for(300ms), std::future_status::timeout) << "the success write waits";
  EXPECT_EQ(p.source.services.repl_log_.last(), seq.load()) << "no replicated write while paused";
  EXPECT_EQ(job_state(), cluster::MigrateState::kStart);
  p.source.services.pause_.Unpause();
  ASSERT_EQ(outcome.wait_for(5s), std::future_status::ready);
  EXPECT_TRUE(outcome.get().ok());
  EXPECT_EQ(job_state(), cluster::MigrateState::kSuccess);
}

// The source demoted: A replicates C, which owns A's former slots.
const std::string kSwapped = kNodeA + " 127.0.0.1 7000 slave " + kNodeC + "\n" + kNodeB +
                             " 127.0.0.1 7001 master - 8192-16383\n" + kNodeC +
                             " 127.0.0.1 7002 master - 0-8191\n";

// D9 + D10 (fix round 1): a manual failover pauses the source while its success publish waits; the demotion
// still stops the job, and the replica publishes no migrated range at UNPAUSE.
TEST(MigrationTransitions, ADemotionWhileTheSuccessRecordWaitsStopsTheJob) {
  MigrationPair p;
  ASSERT_EQ(p.source.Run({"SET", "b", "1"}), kOk);
  migrate::SlotMigrator::Hooks hooks;
  hooks.after_fence = [&p] {
    p.source.services.pause_.Pause(ClientPause::Clock::now() + 60s, ClientPause::Mode::kWrite);
  };
  p.source_migration().migrator().SetHooks(std::move(hooks));
  auto done = std::make_shared<std::promise<Status>>();
  ASSERT_TRUE(
      StartJob(p.source_migration(), {3300, 3300}, [done](const Status& s) { done->set_value(s); }).ok());
  const auto acked = [&p] {
    const std::optional<cluster::ImportJob> import = p.target_cluster.manager->States()->import;
    return import.has_value() && import->state == cluster::ImportState::kSuccess;
  };
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!acked() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(10ms);
  ASSERT_TRUE(acked());  // the job waits at its success publish
  p.source_cluster.Push(kSwapped, 2);
  ASSERT_EQ(p.source.services.replication_.role(), ReplRole::kReplica);
  auto outcome = done->get_future();
  EXPECT_EQ(outcome.wait_for(5s), std::future_status::ready) << "the demotion ends the wait";
  p.source.services.pause_.Unpause();
  ASSERT_EQ(outcome.wait_for(5s), std::future_status::ready);
  EXPECT_EQ(outcome.get().message(), cluster::kErrMigrationCanceled);
  ASSERT_TRUE(p.source_migration().AwaitIdle(5s));
  EXPECT_TRUE(p.source_cluster.manager->States()->migrated.empty()) << "nothing re-published on the replica";
  EXPECT_FALSE(p.source.services.engine_.SlotFenced(3300));
}

TEST(MigrationTransitions, ADemotionWhileATransitionWaitsForUnpauseReleasesTheWorker) {  // D9, fix round 1
  MigrationPair p;
  ASSERT_EQ(p.source.Run({"SET", "b", "1"}), kOk);
  ASSERT_TRUE(RunJob(p, {3300, 3300}).ok());
  ASSERT_EQ(p.source.Run({"CLIENT", "PAUSE", "60000", "WRITE"}), kOk);
  p.source_cluster.Push(kMoved, 2);  // its transition waits for UNPAUSE
  EXPECT_FALSE(p.source_migration().AwaitIdle(200ms));
  p.source_cluster.Push(kSwapped, 3);
  EXPECT_TRUE(p.source_migration().AwaitIdle(5s)) << "a replica writes nothing: no wait";
  EXPECT_EQ(p.source.Run({"CLUSTER", "COUNTKEYSINSLOT", "3300"}), Int(1));  // a replica keeps the data (D21)
  EXPECT_TRUE(p.source_cluster.manager->States()->cleanup.empty());
  ASSERT_EQ(p.source.Run({"CLIENT", "UNPAUSE"}), kOk);
}

}  // namespace
}  // namespace tideskv::test
