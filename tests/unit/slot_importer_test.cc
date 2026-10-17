#include "migrate/slot_importer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cluster/cluster_errors.h"
#include "cluster/migration_record.h"
#include "common/coding.h"
#include "encoding/key_codec.h"
#include "encoding/meta_codec.h"
#include "expire/reaper.h"
#include "migrate/migrate_frames.h"
#include "support/migration_harness.h"
#include "types/errors.h"
#include "types/object_store.h"

namespace tideskv::test {
namespace {

using namespace std::chrono_literals;
using cluster::ImportState;

// Runs CLUSTER IMPORT <range> 0, every chunk of `range` on the source, then IMPORT <range> <end> on `link`.
void Import(MigrationPair& p, SecondClient& link, const std::string& range, cluster::SlotRange r,
            const char* end) {
  ASSERT_EQ(link.Run({"CLUSTER", "IMPORT", range, "0"}), kOk);
  for (const std::string& chunk : RangeChunks(p.source, r)) {
    ASSERT_EQ(link.Run({"CLUSTER", "IMPORTDATA", chunk}), kOk);
  }
  if (end != nullptr) {
    ASSERT_EQ(link.Run({"CLUSTER", "IMPORT", range, end}), kOk);
  }
}

uint64_t OidOf(CommandHarness& h, const std::string& key) {
  return encoding::DecodeMetadata(h.Cf(ColumnFamily::kMeta).at(encoding::MetaKeyFor(0, key)))->meta.oid;
}

// Review Focus #2: a TTL'd collection and string migrated mid-life keep their exact deadlines, and the
// target's reaper reaps them: their expire entries carry the target's oids.
TEST(SlotImporterTest, AnImportedCollectionWithATtlIsReapedOnTheTarget) {
  MigrationPair p;
  ASSERT_EQ(p.target.Run({"HSET", "{foo}pad", "f", "v"}), Int(1));  // the target's oids are already in use
  ASSERT_EQ(p.source.Run({"HSET", "{b}h", "f1", "v1", "f2", "v2"}), Int(2));
  ASSERT_EQ(p.source.Run({"PEXPIRE", "{b}h", "60000"}), Int(1));
  ASSERT_EQ(p.source.Run({"SET", "{b}s", "v", "PX", "60000"}), kOk);
  const int64_t hash_deadline = p.source.ExpireOf(0, "{b}h");
  const int64_t string_deadline = p.source.ExpireOf(0, "{b}s");
  SecondClient link(p.target);
  ASSERT_NO_FATAL_FAILURE(Import(p, link, "3300", {3300, 3300}, "1"));
  EXPECT_EQ(p.target.ExpireOf(0, "{b}h"), hash_deadline);
  EXPECT_EQ(p.target.ExpireOf(0, "{b}s"), string_deadline);
  const uint64_t target_oid = OidOf(p.target, "{b}h");
  EXPECT_NE(target_oid, OidOf(p.source, "{b}h"));
  const auto& expire = p.target.Cf(ColumnFamily::kExpire);
  const auto entry = expire.find(encoding::ExpireKeyFor(0, static_cast<uint64_t>(hash_deadline), "{b}h"));
  ASSERT_NE(entry, expire.end());
  EXPECT_EQ(entry->second, ExpireValue(target_oid));
  EXPECT_EQ(p.target.Run({"HGETALL", "{b}h"}), Arr({Bulk("f1"), Bulk("v1"), Bulk("f2"), Bulk("v2")}));
  p.target.clock().Set(hash_deadline + 1);  // both deadlines are 60 s from the same instant
  ExpireReaper reaper(p.target.services.engine_, ExpireReaper::Options{});
  auto reaped = reaper.RunOnce();
  ASSERT_TRUE(reaped.ok()) << reaped.status().ToString();
  EXPECT_EQ(*reaped, 2u);
  EXPECT_EQ(p.target.Cf(ColumnFamily::kMeta).count(encoding::MetaKeyFor(0, "{b}h")), 0u);
  EXPECT_EQ(p.target.Cf(ColumnFamily::kMeta).count(encoding::MetaKeyFor(0, "{b}s")), 0u);
  EXPECT_EQ(p.target.Cf(ColumnFamily::kSubkey).count(encoding::EncodeSubkey(0, 3300, target_oid, "f1")), 0u);
}

TEST(SlotImporterTest, ImportStartErasesTheRangeAndPublishesStart) {  // D14
  MigrationPair p;
  ASSERT_TRUE(p.target.services.engine_
                  .ApplyImport({{encoding::ReplOpType::kPut, ColumnFamily::kMeta,
                                 encoding::MetaKeyFor(0, "b"), encoding::EncodeMetadata({}, "stale")}})
                  .ok());  // left by an earlier, failed import
  SecondClient link(p.target);
  EXPECT_EQ(link.Run({"CLUSTER", "IMPORT", "3300", "0"}), kOk);
  EXPECT_TRUE(link.session.import_link);
  EXPECT_EQ(p.target.Run({"CLUSTER", "COUNTKEYSINSLOT", "3300"}), Int(0));
  EXPECT_NE(p.target.Run({"CLUSTER", "INFO"}).find("importing_slot(s): 3300\r\nimport_state: start\r\n"),
            std::string::npos);
  auto record = cluster::LoadMigrationRecord(p.target.services.storage_);
  ASSERT_TRUE(record.ok() && record->has_value());
  EXPECT_EQ((*record)->import->state, ImportState::kStart);
  EXPECT_EQ(link.Run({"CLUSTER", "IMPORT", "3300", "0"}), kOk);             // idempotent (Kvrocks)
  EXPECT_EQ(p.target.Run({"GET", "b"}), Err("MOVED 3300 127.0.0.1:7000"));  // still A's: clients go there
}

TEST(SlotImporterTest, EveryTypeArrivesIntactAndTheDigestsMatch) {  // D2 remap at every site
  MigrationPair p;
  ASSERT_EQ(p.target.Run({"SADD", "{foo}pad", "x"}), Int(1));
  ASSERT_EQ(p.source.Run({"ZADD", "{b}z", "1.5", "m1", "-2", "m2"}), Int(2));
  ASSERT_EQ(p.source.Run({"RPUSH", "{b}l", "a", "b", "c"}), Int(3));
  ASSERT_EQ(p.source.Run({"LPOP", "{b}l"}), Bulk("a"));
  ASSERT_EQ(p.source.Run({"SADD", "{b}st", "x", "y"}), Int(2));
  ASSERT_EQ(p.source.Run({"HSET", "{b}h", "f", "v"}), Int(1));
  ASSERT_EQ(p.source.Run({"SET", "b", ""}), kOk);  // gocase: an empty value
  ASSERT_EQ(p.source.Run({"PEXPIRE", "{b}z", "60000"}), Int(1));
  SecondClient link(p.target);
  ASSERT_NO_FATAL_FAILURE(Import(p, link, "3300", {3300, 3300}, "1"));
  EXPECT_EQ(p.target.Run({"ZRANGE", "{b}z", "0", "-1", "WITHSCORES"}),
            Arr({Bulk("m2"), Bulk("-2"), Bulk("m1"), Bulk("1.5")}));
  EXPECT_EQ(p.target.Run({"LRANGE", "{b}l", "0", "-1"}), Arr({Bulk("b"), Bulk("c")}));
  EXPECT_EQ(p.target.Run({"SCARD", "{b}st"}), Int(2));
  EXPECT_EQ(p.target.Run({"GET", "b"}), Bulk(""));
  EXPECT_EQ(p.target.Run({"DEBUG", "DIGEST-SLOTS", "3300", "TTL"}),
            p.source.Run({"DEBUG", "DIGEST-SLOTS", "3300", "TTL"}));
  EXPECT_NE(p.target.Run({"DEBUG", "DIGEST-INDEX"}).find(":1\r\n:0\r\n:0\r\n"), std::string::npos)
      << "one expire entry (the derived one), none missing, no orphan";
}

TEST(SlotImporterTest, SuccessServesTheRangeAndFailureErasesIt) {  // D14
  MigrationPair p;
  ASSERT_EQ(p.source.Run({"SET", "b", "1"}), kOk);
  ASSERT_EQ(p.source.Run({"SET", "bar", "2"}), kOk);  // 5061
  SecondClient link(p.target);
  ASSERT_NO_FATAL_FAILURE(Import(p, link, "3300", {3300, 3300}, "1"));
  EXPECT_FALSE(link.session.import_link);
  EXPECT_EQ(p.target.Run({"GET", "b"}), Bulk("1"));  // imported: served before any push (D6)
  EXPECT_EQ(p.target_cluster.manager->States()->imported, (std::vector<cluster::SlotRange>{{3300, 3300}}));
  EXPECT_EQ(link.Run({"CLUSTER", "IMPORT", "3300", "1"}), kOk);  // idempotent
  ASSERT_NO_FATAL_FAILURE(Import(p, link, "5061", {5061, 5061}, "2"));
  EXPECT_EQ(p.target.Run({"CLUSTER", "COUNTKEYSINSLOT", "5061"}), Int(0));
  EXPECT_NE(p.target.Run({"CLUSTER", "INFO"}).find("importing_slot(s): 5061\r\nimport_state: error\r\n"),
            std::string::npos);
  EXPECT_EQ(link.Run({"CLUSTER", "IMPORT", "5061", "2"}), kOk);  // idempotent
  EXPECT_EQ(link.Run({"CLUSTER", "IMPORT", "5061", "1"}), Err(cluster::kErrImportNotRunning));
  EXPECT_EQ(p.target.Run({"GET", "b"}), Bulk("1")) << "an earlier import stays imported";
}

TEST(SlotImporterTest, LinkLossWhileStartErasesAndReportsError) {  // D14, gocase: gone within 100 ms
  MigrationPair p;
  ASSERT_EQ(p.source.Run({"SET", "b", "1"}), kOk);
  SecondClient link(p.target);
  ASSERT_NO_FATAL_FAILURE(Import(p, link, "3300", {3300, 3300}, nullptr));
  EXPECT_EQ(p.target.Run({"KEYS", "*"}),
            Arr({Bulk("b")}));  // partial imports are visible to keyless commands
  ASSERT_TRUE(link.session.on_close);
  link.session.on_close();  // what ~Connection runs
  ASSERT_TRUE(p.target_migration().AwaitIdle(5s));
  EXPECT_EQ(p.target.Run({"KEYS", "*"}), "*0\r\n");
  EXPECT_EQ(p.target_cluster.manager->States()->import->state, ImportState::kError);
  SecondClient again(p.target);  // a link lost by a replica keeps its data: its new master erases (D8)
  ASSERT_NO_FATAL_FAILURE(Import(p, again, "3300", {3300, 3300}, nullptr));
  p.target.services.replication_.set_role(ReplRole::kReplica);
  again.session.on_close();
  ASSERT_TRUE(p.target_migration().AwaitIdle(5s));
  EXPECT_EQ(p.target.Run({"KEYS", "*"}), Arr({Bulk("b")}));
  EXPECT_EQ(p.target_cluster.manager->States()->import->state, ImportState::kError);
}

TEST(SlotImporterTest, KvrocksRefusalsAndTexts) {  // cluster.cc:348-404, slot_import.cc
  MigrationPair p;
  SecondClient link(p.target);
  EXPECT_EQ(link.Run({"CLUSTER", "IMPORT", "12182", "0"}), Err(cluster::kErrImportOwnSlot));
  EXPECT_EQ(link.Run({"CLUSTER", "IMPORT", "3300", "3"}), Err(cluster::kErrInvalidImportState));
  EXPECT_EQ(link.Run({"CLUSTER", "IMPORT", "3300", "x"}), Err(cluster::kErrInvalidImportState));
  EXPECT_EQ(link.Run({"CLUSTER", "IMPORT", "1 2", "0"}), Err(cluster::kErrSlotRangeInvalid));
  EXPECT_EQ(link.Run({"CLUSTER", "IMPORT", "3300", "1"}),
            Err(std::string(cluster::kErrImportMismatchPrefix) + "-1" +
                std::string(cluster::kImportMismatchMiddle) + "3300"));
  ASSERT_EQ(link.Run({"CLUSTER", "IMPORT", "3300", "0"}), kOk);
  SecondClient other(p.target);
  EXPECT_EQ(other.Run({"CLUSTER", "IMPORT", "3301", "0"}),
            Err(std::string(cluster::kErrOneImportPrefix) + "3300"));
  EXPECT_EQ(other.Run({"CLUSTER", "IMPORT", "3301", "1"}),
            Err(std::string(cluster::kErrImportMismatchPrefix) + "3300" +
                std::string(cluster::kImportMismatchMiddle) + "3301"));
  CommandHarness fresh;  // no topology yet: "myself" is unknown
  ClusterHarness c(fresh, 7001);
  EXPECT_EQ(fresh.Run({"CLUSTER", "IMPORT", "3300", "0"}), Err(cluster::kErrSlaveCantImport));
}

TEST(SlotImporterTest, ImportDataOutsideTheImportLinkIsRefused) {  // D14
  MigrationPair p;
  SecondClient link(p.target);
  EXPECT_EQ(link.Run({"CLUSTER", "IMPORTDATA", "x"}), Err(cluster::kErrNotImportLink));
  ASSERT_EQ(link.Run({"CLUSTER", "IMPORT", "3300", "0"}), kOk);
  SecondClient other(p.target);
  EXPECT_EQ(other.Run({"CLUSTER", "IMPORTDATA", "x"}), Err(cluster::kErrNotImportLink));
}

TEST(SlotImporterTest, ImportIsAWriteForReadonlyAndClientPause) {  // D14, M7a D21
  MigrationPair p;
  SecondClient link(p.target);
  p.target.services.replication_.set_role(ReplRole::kReplica);
  EXPECT_EQ(link.Run({"CLUSTER", "IMPORT", "3300", "0"}), Err(kErrReadOnlyReplica));
  p.target.services.replication_.set_role(ReplRole::kMaster);
  ASSERT_EQ(p.target.Run({"CLIENT", "PAUSE", "60000", "WRITE"}), kOk);
  EXPECT_EQ(link.Run({"CLUSTER", "IMPORT", "3300", "0"}), "");  // postponed
  EXPECT_EQ(p.target.Run({"CLUSTER", "INFO"}).find("importing_slot"), std::string::npos);
  ASSERT_EQ(p.target.Run({"CLIENT", "UNPAUSE"}), kOk);
  EXPECT_EQ(link.RunPostponed(), kOk);
}

TEST(SlotImporterTest, AnOpOutsideTheRangeFailsTheImport) {  // D2 step 2
  MigrationPair p;
  ASSERT_EQ(p.source.Run({"SET", "bar", "1"}), kOk);  // 5061
  SecondClient link(p.target);
  ASSERT_EQ(link.Run({"CLUSTER", "IMPORT", "3300", "0"}), kOk);
  const std::vector<std::string> wrong = RangeChunks(p.source, {5061, 5061});
  ASSERT_EQ(wrong.size(), 1u);
  EXPECT_EQ(link.Run({"CLUSTER", "IMPORTDATA", wrong[0]}), Err(kErrStorage));
  EXPECT_EQ(p.target_cluster.manager->States()->import->state, ImportState::kError);
  EXPECT_EQ(p.target.Run({"CLUSTER", "COUNTKEYSINSLOT", "5061"}), Int(0));
  EXPECT_EQ(link.Run({"CLUSTER", "IMPORTDATA", wrong[0]}), Err(cluster::kErrImportNotRunning));
  EXPECT_EQ(link.Run({"CLUSTER", "IMPORTDATA", "not a chunk"}), Err(cluster::kErrImportNotRunning));
}

TEST(SlotImporterTest, AnOverlappingImportReleasesMyMigratedRangeAndBlock) {  // D14 step 3, D15
  MigrationPair p;  // A migrated 3300 to B, B owns it now, and B migrates it back to A
  const std::string b_owns = kNodeA + " 127.0.0.1 7000 master - 0-3299 3301-8191\n" + kNodeB +
                             " 127.0.0.1 7001 master - 3300 8192-16383\n" + kNodeC +
                             " 127.0.0.1 7002 slave " + kNodeA + "\n";
  ASSERT_TRUE(p.source_migration()
                  .Mutate([](cluster::SlotStates& s) {
                    s.job = cluster::SourceJob{
                        {3300, 3300}, kNodeB, "127.0.0.1", 7001, cluster::MigrateState::kSuccess};
                    s.migrated.push_back({{3300, 3300}, kNodeB, "127.0.0.1", 7001});
                    cluster::SetFenced(s, {3300, 3300}, true);
                  })
                  .ok());
  p.source_cluster.Push(b_owns, 2);
  SecondClient link(p.source);
  ASSERT_EQ(link.Run({"CLUSTER", "IMPORT", "3300", "0"}), kOk);
  const auto s = p.source_cluster.manager->States();
  EXPECT_TRUE(s->migrated.empty());
  EXPECT_FALSE(s->Fenced(3300));
  EXPECT_FALSE(p.source.services.engine_.SlotFenced(3300));
  EXPECT_FALSE(s->job.has_value()) << "an overlapping IMPORT ends the success block (D15)";
  EXPECT_EQ(s->import->state, ImportState::kStart);
}

// Task 2 review: RemapOp passes a malformed collection meta through unremapped; DerivedExpire rejects it, so
// the import fails instead of storing a source oid. The chunk is applied all or nothing.
TEST(SlotImporterTest, AMalformedMetaFailsTheImport) {
  MigrationPair p;
  encoding::Metadata hash;
  hash.type = encoding::ValueType::kHash;
  hash.oid = 5;
  hash.size = 1;
  const std::string chunk =
      migrate::EncodeChunks({{encoding::ReplOpType::kPut, ColumnFamily::kMeta,
                              encoding::MetaKeyFor(0, "{b}ok"), encoding::EncodeMetadata({}, "v")},
                             {encoding::ReplOpType::kPut, ColumnFamily::kMeta,
                              encoding::MetaKeyFor(0, "{b}h"), encoding::EncodeMetadata(hash, {}) + "x"}},
                            16 * 1024)[0];
  SecondClient link(p.target);
  ASSERT_EQ(link.Run({"CLUSTER", "IMPORT", "3300", "0"}), kOk);
  EXPECT_EQ(link.Run({"CLUSTER", "IMPORTDATA", chunk}), Err(kErrStorage));
  EXPECT_EQ(p.target_cluster.manager->States()->import->state, ImportState::kError);
  EXPECT_EQ(p.target.Run({"CLUSTER", "COUNTKEYSINSLOT", "3300"}), Int(0));
  EXPECT_EQ(p.target.Run({"KEYS", "*"}), "*0\r\n");
}

// Writer's note 7: FLUSH* on the target fails a running import on the worker (erase + error).
TEST(SlotImporterTest, AFlushFailsTheRunningImportOnTheWorker) {
  MigrationPair p;
  ASSERT_EQ(p.source.Run({"SET", "b", "1"}), kOk);
  SecondClient link(p.target);
  ASSERT_NO_FATAL_FAILURE(Import(p, link, "3300", {3300, 3300}, nullptr));
  ASSERT_TRUE(p.target_migration().importer().running());
  p.target_migration().importer().FailRunning();
  ASSERT_TRUE(p.target_migration().AwaitIdle(5s));
  EXPECT_FALSE(p.target_migration().importer().running());
  EXPECT_EQ(p.target.Run({"KEYS", "*"}), "*0\r\n");
  EXPECT_EQ(p.target_cluster.manager->States()->import->state, ImportState::kError);
  const std::vector<std::string> chunks = RangeChunks(p.source, {3300, 3300});
  ASSERT_FALSE(chunks.empty());
  EXPECT_EQ(link.Run({"CLUSTER", "IMPORTDATA", chunks[0]}), Err(cluster::kErrImportNotRunning));
}

// Blocks `m`'s worker until the returned flag is set (or Shutdown).
std::shared_ptr<std::atomic<bool>> BlockWorker(migrate::MigrationService& m) {
  auto release = std::make_shared<std::atomic<bool>>(false);
  m.Post([release, &m] {
    while (!release->load() && !m.stopping()) std::this_thread::sleep_for(1ms);
  });
  return release;
}

// Controller ruling (Task 7 review): IMPORT start waits for the migrate worker, including the half-import
// erase a promotion's restore posts, before it erases its range, so that erase never lands after the new
// import's chunks.
TEST(SlotImporterTest, ImportStartWaitsForAPromotionsHalfImportEraseBeforeErasing) {
  MigrationPair p;
  ASSERT_TRUE(p.target.services.engine_
                  .ApplyImport({{encoding::ReplOpType::kPut, ColumnFamily::kMeta,
                                 encoding::MetaKeyFor(0, "b"), encoding::EncodeMetadata({}, "partial")}})
                  .ok());
  cluster::MigrationRecord half;
  half.import = cluster::ImportJob{{3300, 3300}, ImportState::kStart};
  ASSERT_TRUE(p.target.services.repl_log_.CommitStandalone({cluster::MigrationRecordPut(half)}).ok());
  ASSERT_EQ(p.source.Run({"SET", "b", "new"}), kOk);
  auto release = BlockWorker(p.target_migration());
  p.target_migration().OnPromoted();  // the restore and its half-import erase queue behind the block
  std::atomic<bool> released{false};
  SecondClient link(p.target);
  std::string reply;
  bool returned_after_release = false;
  std::thread importer([&] {
    reply = link.Run({"CLUSTER", "IMPORT", "3300", "0"});
    returned_after_release = released.load();
  });
  std::this_thread::sleep_for(100ms);
  released = true;
  *release = true;
  importer.join();
  ASSERT_EQ(reply, kOk);
  EXPECT_TRUE(returned_after_release) << "IMPORT start ran before the worker drained";
  for (const std::string& chunk : RangeChunks(p.source, {3300, 3300})) {
    ASSERT_EQ(link.Run({"CLUSTER", "IMPORTDATA", chunk}), kOk);
  }
  ASSERT_EQ(link.Run({"CLUSTER", "IMPORT", "3300", "1"}), kOk);
  ASSERT_TRUE(p.target_migration().AwaitIdle(5s));
  EXPECT_EQ(p.target.Run({"GET", "b"}), Bulk("new"));
}

// Controller ruling S2(b): an overlapping cleanup is waited for on the worker; IMPORT start proceeds once it
// is done.
TEST(SlotImporterTest, ImportStartWaitsForAnOverlappingCleanup) {
  MigrationPair p;
  migrate::MigrationService& m = p.target_migration();
  ASSERT_TRUE(m.Mutate([](cluster::SlotStates& s) { s.cleanup.push_back({3300, 3300}); }).ok());
  auto release = BlockWorker(m);
  m.Post([&m] { m.RunCleanup({3300, 3300}); });
  std::thread releaser([release] {
    std::this_thread::sleep_for(50ms);
    *release = true;
  });
  SecondClient link(p.target);
  EXPECT_EQ(link.Run({"CLUSTER", "IMPORT", "3300", "0"}), kOk);
  releaser.join();
  EXPECT_TRUE(p.target_cluster.manager->States()->cleanup.empty());
  EXPECT_EQ(p.target_cluster.manager->States()->import->state, ImportState::kStart);
}

TEST(SlotImporterTest, ACleanupThatNeverEndsAnswersCleanupPending) {  // S2(b)
  MigrationPair p;
  migrate::MigrationService& m = p.target_migration();
  ASSERT_TRUE(m.Mutate([](cluster::SlotStates& s) { s.cleanup.push_back({3300, 3301}); }).ok());
  m.Shutdown();  // nothing will ever clean it: AwaitIdle returns at once
  SecondClient link(p.target);
  EXPECT_EQ(link.Run({"CLUSTER", "IMPORT", "3300", "0"}), Err(cluster::kErrImportCleanupPending));
  EXPECT_FALSE(p.target_cluster.manager->States()->import.has_value());
  EXPECT_FALSE(link.session.import_link);
}

}  // namespace
}  // namespace tideskv::test
