#include "migrate/migration_service.h"

#include <gtest/gtest.h>
#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "cluster/migration_record.h"
#include "support/cluster_harness.h"
#include "types/errors.h"

namespace tideskv::test {
namespace {

using namespace std::chrono_literals;
using cluster::MigrateState;

struct ServiceTest : ::testing::Test {
  ServiceTest() { c.Push(kThreeNodes, 1); }  // node A owns 0-8191 ("b" 3300, "bar" 5061)
  std::unique_ptr<migrate::MigrationService> Make() {
    return std::make_unique<migrate::MigrationService>(MigrationDepsOf(h, *c.manager));
  }
  std::optional<cluster::MigrationRecord> Record() {
    auto r = cluster::LoadMigrationRecord(h.services.storage_);
    EXPECT_TRUE(r.ok());
    return r.ok() ? *r : std::nullopt;
  }
  void Persist(const cluster::MigrationRecord& r) {
    ASSERT_TRUE(h.services.repl_log_.CommitStandalone({cluster::MigrationRecordPut(r)}).ok());
  }
  CommandHarness h;
  ClusterHarness c{h};
};

TEST_F(ServiceTest, MutateFencesPersistsThePersistedPartAndPublishes) {  // D8
  auto svc = Make();
  ASSERT_TRUE(svc->Mutate([](cluster::SlotStates& s) {
                   s.job = cluster::SourceJob{{3300, 3300}, kNodeB, "127.0.0.1", 7001, MigrateState::kStart};
                 })
                  .ok());
  EXPECT_EQ(c.manager->States()->job->state, MigrateState::kStart);
  EXPECT_FALSE(Record().has_value()) << "a running job is never persisted";
  const uint64_t before = h.services.repl_log_.last();
  uint64_t fence_seq = 0;
  ASSERT_TRUE(svc->Mutate(
                     [](cluster::SlotStates& s) {
                       s.job->state = MigrateState::kSuccess;
                       s.migrated.push_back({{3300, 3300}, kNodeB, "127.0.0.1", 7001});
                       cluster::SetFenced(s, {3300, 3300}, true);
                     },
                     true, &fence_seq)
                  .ok());
  EXPECT_EQ(fence_seq, before);  // read inside the barrier, before the record write
  EXPECT_TRUE(h.services.engine_.SlotFenced(3300));
  EXPECT_EQ(h.services.repl_log_.last(), before + 1) << "the record is replicated";
  ASSERT_TRUE(Record().has_value());
  EXPECT_EQ(Record()->migrated.size(), 1u);
  EXPECT_EQ(h.Run({"GET", "b"}), Err("MOVED 3300 127.0.0.1:7001"));
}

TEST_F(ServiceTest, APersistFailureLeavesTheStatesAndTheFenceUnchanged) {
  auto svc = Make();
  h.services.storage_.fail_commit_in = 1;
  const Status st = svc->Mutate([](cluster::SlotStates& s) {
    s.migrated.push_back({{3300, 3300}, kNodeB, "127.0.0.1", 7001});
    cluster::SetFenced(s, {3300, 3300}, true);
  });
  EXPECT_FALSE(st.ok());
  EXPECT_TRUE(c.manager->States()->migrated.empty());
  EXPECT_FALSE(h.services.engine_.SlotFenced(3300));
}

TEST_F(ServiceTest, RestoreFencesMigratedRangesAndResumesTheCleanup) {  // D8 boot
  ASSERT_EQ(h.Run({"SET", "bar", "stale"}), kOk);                       // 5061: cleanup pending
  ASSERT_EQ(h.Run({"SET", "b", "migrated"}), kOk);                      // 3300: migrated, not yet pushed away
  cluster::MigrationRecord r;
  r.job = cluster::SourceJob{{3300, 3300}, kNodeB, "127.0.0.1", 7001, MigrateState::kSuccess};
  r.migrated = {{{3300, 3300}, kNodeB, "127.0.0.1", 7001}};
  r.cleanup = {{5061, 5061}};
  Persist(r);
  auto svc = Make();
  ASSERT_TRUE(svc->Restore().ok());
  EXPECT_TRUE(h.services.engine_.SlotFenced(3300));
  EXPECT_EQ(h.Run({"GET", "b"}), Err("MOVED 3300 127.0.0.1:7001"));
  ASSERT_TRUE(svc->AwaitIdle(5s));
  EXPECT_EQ(h.Run({"CLUSTER", "COUNTKEYSINSLOT", "5061"}), Int(0));
  EXPECT_FALSE(h.services.engine_.SlotFenced(5061));
  EXPECT_TRUE(c.manager->States()->cleanup.empty());
  EXPECT_TRUE(Record()->cleanup.empty());
  EXPECT_EQ(Record()->migrated.size(), 1u);  // 3300 stays until a push moves it away (D7)
  EXPECT_EQ(h.Run({"CLUSTER", "COUNTKEYSINSLOT", "3300"}), Int(1));
}

TEST_F(ServiceTest, RestoreErasesAHalfImport) {  // D8: importing start -> erase + error
  ASSERT_EQ(h.services.engine_
                .ApplyImport({{encoding::ReplOpType::kPut, ColumnFamily::kMeta,
                               encoding::MetaKeyFor(0, "foo"), encoding::EncodeMetadata({}, "partial")}})
                .ok(),
            true);  // slot 12182 is B's: half imported here
  cluster::MigrationRecord r;
  r.import = cluster::ImportJob{{12182, 12182}, cluster::ImportState::kStart};
  Persist(r);
  auto svc = Make();
  ASSERT_TRUE(svc->Restore().ok());
  EXPECT_EQ(h.Run({"CLUSTER", "COUNTKEYSINSLOT", "12182"}), Int(0));
  EXPECT_EQ(c.manager->States()->import->state, cluster::ImportState::kError);
  EXPECT_EQ(Record()->import->state, cluster::ImportState::kError);
}

TEST_F(ServiceTest, ARecordThatFailsToDecodeStopsTheRestore) {
  ASSERT_TRUE(h.services.repl_log_
                  .CommitStandalone({{encoding::ReplOpType::kPut, ColumnFamily::kSys,
                                      std::string(cluster::kSysMigration), "garbage"}})
                  .ok());
  EXPECT_EQ(Make()->Restore().code(), StatusCode::kCorruption);
}

TEST_F(ServiceTest, OnPromotedRestoresOnTheWorker) {
  cluster::MigrationRecord r;
  r.imported = {{12182, 12182}};
  Persist(r);
  auto svc = Make();
  EXPECT_EQ(h.Run({"SET", "foo", "1"}), Err("MOVED 12182 127.0.0.1:7001"));
  svc->OnPromoted();
  ASSERT_TRUE(svc->AwaitIdle(5s));
  EXPECT_EQ(h.Run({"SET", "foo", "1"}), kOk) << "an imported range is served after the promotion";
}

TEST_F(ServiceTest, TheWorkerRunsTasksInOrderAndShutdownDropsLatePosts) {
  auto svc = Make();
  std::vector<int> ran;
  for (int i = 0; i < 5; ++i) svc->Post([&ran, i] { ran.push_back(i); });
  ASSERT_TRUE(svc->AwaitIdle(5s));
  EXPECT_EQ(ran, (std::vector<int>{0, 1, 2, 3, 4}));
  svc->Shutdown();
  svc->Post([&ran] { ran.push_back(99); });
  EXPECT_TRUE(svc->AwaitIdle(1s));
  EXPECT_EQ(ran.size(), 5u);
  EXPECT_TRUE(svc->stopping());
}

TEST_F(ServiceTest, ARestoreRewritesTheRecordOnlyWhenItChanges) {  // no replog write per boot
  cluster::MigrationRecord r;
  r.imported = {{12182, 12182}};
  Persist(r);
  const uint64_t before = h.services.repl_log_.last();
  ASSERT_TRUE(Make()->Restore().ok());
  EXPECT_EQ(h.services.repl_log_.last(), before);
  r.import = cluster::ImportJob{{12539, 12539}, cluster::ImportState::kStart};
  Persist(r);
  ASSERT_TRUE(Make()->Restore().ok());
  EXPECT_EQ(Record()->import->state, cluster::ImportState::kError) << "the half import is rewritten";
}

TEST_F(ServiceTest, AFailingEditChangesNothing) {  // the fallible overload Restore uses
  auto svc = Make();
  const Status st = svc->Mutate([](cluster::SlotStates& s) -> Status {
    s.migrated.push_back({{3300, 3300}, kNodeB, "127.0.0.1", 7001});
    cluster::SetFenced(s, {3300, 3300}, true);
    return Status::Corruption("refused");
  });
  EXPECT_EQ(st.code(), StatusCode::kCorruption);
  EXPECT_TRUE(c.manager->States()->migrated.empty());
  EXPECT_FALSE(h.services.engine_.SlotFenced(3300));
  EXPECT_FALSE(Record().has_value());
}

// Controller ruling S3: Restore loads the record inside one Mutate, so a MIGRATE or an IMPORT that lands
// between OnPromoted's post and the restore is neither overwritten nor taken for a half import.
TEST_F(ServiceTest, AMutateBetweenThePromotionAndTheRestoreIsKept) {
  cluster::MigrationRecord r;
  r.imported = {{12182, 12182}};
  Persist(r);
  auto svc = Make();
  std::atomic<bool> release{false};
  svc->Post([&release] {
    while (!release.load()) std::this_thread::sleep_for(1ms);
  });
  svc->OnPromoted();  // queued behind the blocker
  ASSERT_TRUE(svc->Mutate([](cluster::SlotStates& s) {
                   s.job = cluster::SourceJob{{3300, 3300}, kNodeB, "127.0.0.1", 7001, MigrateState::kStart};
                   cluster::SetFenced(s, {3300, 3300}, true);
                   s.import = cluster::ImportJob{{12539, 12539}, cluster::ImportState::kStart};
                 })
                  .ok());
  release = true;
  ASSERT_TRUE(svc->AwaitIdle(5s));
  const std::shared_ptr<const cluster::SlotStates> s = c.manager->States();
  ASSERT_TRUE(s->job.has_value());
  EXPECT_EQ(s->job->state, MigrateState::kStart) << "the running job survives the restore";
  EXPECT_TRUE(h.services.engine_.SlotFenced(3300));
  ASSERT_TRUE(s->import.has_value());
  EXPECT_EQ(s->import->state, cluster::ImportState::kStart) << "a live import is not a half import";
  EXPECT_EQ(s->imported, (std::vector<cluster::SlotRange>{{12182, 12182}}));
  EXPECT_EQ(Record()->import->state, cluster::ImportState::kStart);
}

// Captures what is logged while alive.
class LogCapture {
 public:
  LogCapture()
      : sink_(std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(64)), saved_(spdlog::default_logger()) {
    spdlog::set_default_logger(std::make_shared<spdlog::logger>("capture", sink_));
  }
  ~LogCapture() { spdlog::set_default_logger(saved_); }
  LogCapture(const LogCapture&) = delete;
  LogCapture& operator=(const LogCapture&) = delete;
  bool Contains(std::string_view text) const {
    for (const std::string& line : sink_->last_formatted()) {
      if (line.find(text) != std::string::npos) return true;
    }
    return false;
  }

 private:
  std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> sink_;
  std::shared_ptr<spdlog::logger> saved_;
};

TEST_F(ServiceTest, ACleanupWaitsOutABusyScriptWithoutFailing) {  // controller ruling: BUSY is retried
  ASSERT_EQ(h.Run({"SET", "bar", "stale"}), kOk);                 // 5061
  auto svc = Make();
  ASSERT_TRUE(svc->Mutate([](cluster::SlotStates& s) {
                   s.cleanup.push_back({5061, 5061});
                   cluster::SetFenced(s, {5061, 5061}, true);
                 })
                  .ok());
  Engine& engine = h.services.engine_;
  engine.SetBusyGuard([] { return true; }, std::string(kErrBusyScript));
  std::atomic<int> held{0};
  std::atomic<bool> release{false};
  std::thread script([&] {  // a timed-out script holds the gate
    auto batch = engine.BeginBatch();
    held = batch.ok() ? 1 : 2;
    while (!release.load()) std::this_thread::sleep_for(1ms);
  });
  while (held.load() == 0) std::this_thread::yield();
  LogCapture log;
  svc->Post([&svc] { svc->RunCleanup({5061, 5061}); });
  std::this_thread::sleep_for(50ms);  // the eraser meets BUSY
  release = true;
  script.join();
  ASSERT_EQ(held.load(), 1);
  ASSERT_TRUE(svc->AwaitIdle(5s));
  EXPECT_FALSE(log.Contains("cleanup of slot")) << "BUSY is waited out, not a failure";
  EXPECT_TRUE(c.manager->States()->cleanup.empty());
  EXPECT_FALSE(engine.SlotFenced(5061));
  EXPECT_EQ(h.Run({"CLUSTER", "COUNTKEYSINSLOT", "5061"}), Int(0));
}

}  // namespace
}  // namespace tideskv::test
