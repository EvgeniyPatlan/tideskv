#include "cluster/cluster_manager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "cluster/cluster_errors.h"
#include "cluster/cluster_store.h"
#include "storage/memory_storage.h"
#include "types/errors.h"

namespace tideskv::cluster {
namespace {

const std::string kA(40, 'a');
const std::string kB(40, 'b');
const std::string kC(40, 'c');
const std::string kNodes = kA + " 127.0.0.1 7000 master - 0-8191\n" + kB +
                           " 127.0.0.1 7001 master - 8192-16383\n" + kC + " 127.0.0.1 7002 slave " + kA +
                           "\n";

// Records the role hooks the way ReplicationManager behaves: BecomeReplica of the same master is a no-op.
struct ClusterManagerTest : ::testing::Test {
  std::unique_ptr<ClusterManager> Make(uint16_t port = 7000, std::string bind = "127.0.0.1",
                                       Storage* on = nullptr) {
    ClusterManager::Options o;
    o.port = port;
    o.bind = std::move(bind);
    o.local_addresses = [] { return std::vector<std::string>{"127.0.0.1", "10.0.0.5"}; };
    o.is_replica = [this] { return replica; };
    o.become_replica = [this](const std::string& host, uint16_t p) {
      const std::string to = host + ":" + std::to_string(p);
      if (replica && master == to) return false;
      replica = true;
      master = to;
      events.push_back("replicaof " + to);
      return true;
    };
    o.become_master = [this] {
      if (!promote_status.ok()) return promote_status;
      replica = false;
      events.push_back("master");
      return Status::Ok();
    };
    o.make_durable = [this] {
      ++checkpoints;
      return Status::Ok();
    };
    o.on_publish = [this](const std::shared_ptr<const Topology>& t) { published.push_back(t->version); };
    auto m = std::make_unique<ClusterManager>(on != nullptr ? *on : storage, std::move(o));
    EXPECT_TRUE(m->Load().ok());
    return m;
  }

  InMemoryStorage storage;
  bool replica = false;
  std::string master;
  Status promote_status = Status::Ok();
  std::vector<std::string> events;
  std::vector<int64_t> published;
  int checkpoints = 0;
};

TEST_F(ClusterManagerTest, StartsUninitialised) {
  auto m = Make();
  EXPECT_EQ(m->Current()->version, kNoVersion);
  EXPECT_TRUE(m->Current()->myid.empty());
  EXPECT_FALSE(m->BootMaster().has_value());
  EXPECT_EQ(published, (std::vector<int64_t>{kNoVersion}));  // Load publishes what it read
}

TEST_F(ClusterManagerTest, VersionRulesAreKvrocks) {  // D5, cluster.cc:148-160
  auto m = Make();
  EXPECT_EQ(m->SetNodes(kNodes, -1, false).message(), kErrInvalidClusterVersion);
  EXPECT_EQ(m->SetNodes(kNodes, -1, true).message(), kErrInvalidClusterVersion);
  ASSERT_TRUE(m->SetNodes(kNodes, 2, false).ok());
  EXPECT_EQ(m->SetNodes(kNodes, 1, false).message(), kErrInvalidClusterVersion);
  EXPECT_TRUE(m->SetNodes("not even parsed", 2, false).ok());  // the same version: +OK, no change
  EXPECT_EQ(m->Current()->nodes.size(), 3u);
  EXPECT_EQ(published, (std::vector<int64_t>{kNoVersion, 2}));
  ASSERT_TRUE(m->SetNodes(kA + " 127.0.0.1 7000 master - 0-16383", 1, /*force=*/true).ok());
  EXPECT_EQ(m->Current()->version, 1);
  EXPECT_EQ(m->Current()->nodes.size(), 1u);
}

TEST_F(ClusterManagerTest, AParseErrorChangesNothing) {
  auto m = Make();
  ASSERT_TRUE(m->SetNodes(kNodes, 2, false).ok());
  EXPECT_EQ(m->SetNodes(kA + " h 1 master - 5-5", 3, false).message(), kErrSlotOutOfRange);
  EXPECT_EQ(m->Current()->version, 2);
}

TEST_F(ClusterManagerTest, MyIdIsTheFirstNodeOnThisPortAndBindInStringOrder) {  // D6
  auto m = Make(7000);
  ASSERT_TRUE(m->SetNodes(kNodes, 1, false).ok());
  EXPECT_EQ(m->Current()->myid, kA);
  EXPECT_EQ(m->Current()->myself, 0);
  InMemoryStorage other;
  auto wild = Make(7000, "0.0.0.0", &other);
  ASSERT_TRUE(
      wild->SetNodes(kB + " 10.0.0.5 7000 master - 0-1\n" + kA + " 127.0.0.1 7000 master - 2", 1, false)
          .ok());
  EXPECT_EQ(wild->Current()->myid, kB);  // both are local; the first line wins
}

TEST_F(ClusterManagerTest, SetNodeIdWinsUntilAForcedSetnodes) {
  auto m = Make(7000);
  EXPECT_EQ(m->SetNodeId("short").message(), kErrInvalidNodeId);
  ASSERT_TRUE(m->SetNodeId(kB).ok());
  ASSERT_TRUE(m->SetNodes(kNodes, 1, false).ok());
  EXPECT_EQ(m->Current()->myid, kB);  // not re-detected: myid was set
  ASSERT_TRUE(m->SetNodes(kNodes, 1, /*force=*/true).ok());
  EXPECT_EQ(m->Current()->myid, kA);  // force re-detects (Kvrocks)
}

// Final review minor 3: a checkpoint excludes every commit, and the controller resends SETNODEID on every
// sync, so an unchanged id re-applies the role but persists, checkpoints and publishes nothing.
TEST_F(ClusterManagerTest, AnUnchangedNodeIdAppliesTheRoleWithoutACheckpoint) {
  auto m = Make(7002);
  ASSERT_TRUE(m->SetNodes(kNodes, 1, false).ok());
  ASSERT_TRUE(m->SetNodeId(kC).ok());
  const int checkpoints_before = checkpoints;
  const std::vector<int64_t> published_before = published;
  replica = false;  // the role drifted (e.g. a failed earlier apply): SETNODEID re-applies it
  ASSERT_TRUE(m->SetNodeId(kC).ok());
  EXPECT_EQ(checkpoints, checkpoints_before);
  EXPECT_EQ(published, published_before);
  EXPECT_TRUE(replica);
  EXPECT_EQ(m->Current()->myid, kC);
}

TEST_F(ClusterManagerTest, ASlaveFollowsItsMasterAndRepeatedPushesAreNoOps) {  // D10
  auto m = Make(7002);
  ASSERT_TRUE(m->SetNodes(kNodes, 1, false).ok());
  ASSERT_TRUE(m->SetNodes(kNodes, 2, false).ok());
  EXPECT_EQ(events, (std::vector<std::string>{"replicaof 127.0.0.1:7000"}));
  const std::optional<Endpoint> boot = m->BootMaster();
  ASSERT_TRUE(boot.has_value());
  EXPECT_EQ(boot->port, 7000);
}

TEST_F(ClusterManagerTest, AMasterThatWasAReplicaIsPromoted) {
  replica = true;
  auto m = Make(7000);
  ASSERT_TRUE(m->SetNodes(kNodes, 1, false).ok());
  EXPECT_EQ(events, (std::vector<std::string>{"master"}));
}

TEST_F(ClusterManagerTest, ANodeRemovedFromTheTopologyStopsReplicating) {  // Kvrocks #2643
  auto m = Make(7002);
  ASSERT_TRUE(m->SetNodes(kNodes, 1, false).ok());
  ASSERT_TRUE(m->SetNodes(kA + " 127.0.0.1 7000 master - 0-16383", 2, false).ok());
  EXPECT_EQ(events, (std::vector<std::string>{"replicaof 127.0.0.1:7000", "master"}));
  EXPECT_EQ(m->Current()->Myself(), nullptr);
}

TEST_F(ClusterManagerTest, ASlaveOfAnUnknownMasterKeepsItsRole) {
  auto m = Make(7002);
  ASSERT_TRUE(m->SetNodes(kC + " 127.0.0.1 7002 slave " + std::string(40, 'z'), 1, false).ok());
  EXPECT_TRUE(events.empty());
}

TEST_F(ClusterManagerTest, AFailedPromotionKeepsTheOldVersionInMemoryAndOnDisk) {  // D7
  replica = true;
  promote_status = Status::UserError(std::string(kErrLoading));
  auto m = Make(7000);
  EXPECT_EQ(m->SetNodes(kNodes, 1, false).message(),
            "ERR failed to set master-replica replication: LOADING Redis is loading the dataset in memory");
  EXPECT_EQ(m->Current()->version, kNoVersion);
  EXPECT_EQ((*LoadClusterCfg(storage))->version, kNoVersion);
  promote_status = Status::IOError("disk on fire");
  EXPECT_EQ(m->SetNodes(kNodes, 1, false).message(),
            "ERR failed to set master-replica replication: storage error");
  promote_status = Status::Ok();
  ASSERT_TRUE(m->SetNodes(kNodes, 1, false).ok());  // the controller's re-push succeeds
  EXPECT_EQ(m->Current()->version, 1);
}

TEST_F(ClusterManagerTest, SlotsMissingFromANewTopologyAreNotServed) {  // D3: Kvrocks keeps stale owners
  auto m = Make();
  ASSERT_TRUE(m->SetNodes(kA + " 127.0.0.1 7000 master - 0-16383", 1, false).ok());
  ASSERT_TRUE(m->SetNodes(kA + " 127.0.0.1 7000 master - 0-8191", 2, false).ok());
  EXPECT_NE(m->Current()->OwnerOf(8191), nullptr);
  EXPECT_EQ(m->Current()->OwnerOf(8192), nullptr);
  EXPECT_EQ(m->Current()->AssignedSlots(), 8192u);
}

TEST_F(ClusterManagerTest, SetSlotIsAStrictCasThatMovesOnlyTheNamedSlots) {  // D5, cluster.cc:81-146
  auto m = Make();
  EXPECT_EQ(m->SetSlot({{0, 0}}, kB, 0).message(), kErrInvalidClusterVersion);  // uninitialised
  ASSERT_TRUE(m->SetNodes(kNodes, 1, false).ok());
  EXPECT_EQ(m->SetSlot({{0, 0}}, kB, 3).message(), kErrInvalidClusterVersion);
  EXPECT_EQ(m->SetSlot({{0, 0}}, std::string(40, 'z'), 2).message(), kErrNoSuchNode);
  EXPECT_EQ(m->SetSlot({{0, 0}}, kC, 2).message(), kErrNotAMaster);
  ASSERT_TRUE(m->SetSlot({{0, 0}, {5, 6}}, kB, 2).ok());
  const auto t = m->Current();
  EXPECT_EQ(t->version, 2);
  EXPECT_EQ(t->OwnerOf(0)->id, kB);
  EXPECT_EQ(t->OwnerOf(1)->id, kA);
  EXPECT_EQ(t->OwnerOf(6)->id, kB);
  EXPECT_EQ(t->Size(), 2u);
  EXPECT_TRUE(events.empty());  // SETSLOT changes no role
}

TEST_F(ClusterManagerTest, ResetNeedsAnEmptyKeyspaceThenForgetsEverything) {  // cluster.cc:960-1000
  auto m = Make(7002);
  ASSERT_TRUE(m->SetNodes(kNodes, 1, false).ok());
  EXPECT_EQ(m->Reset(/*keyspace_empty=*/false).message(), kErrResetNotEmpty);
  ASSERT_TRUE(m->Reset(true).ok());
  EXPECT_EQ(events.back(), "master");
  EXPECT_EQ(m->Current()->version, kNoVersion);
  EXPECT_TRUE(m->Current()->myid.empty());
  EXPECT_EQ((*LoadClusterCfg(storage))->version, kNoVersion);
}

TEST_F(ClusterManagerTest, TheTopologyIsPersistedDurablyAndReloadedWithItsBootRole) {  // D20
  auto m = Make(7002);
  ASSERT_TRUE(m->SetNodeId(kC).ok());
  ASSERT_TRUE(m->SetNodes(kNodes, 5, false).ok());
  EXPECT_EQ(checkpoints, 2);
  ClusterManager::Options o;  // a restart: same storage, no hooks needed to load
  ClusterManager reloaded(storage, std::move(o));
  ASSERT_TRUE(reloaded.Load().ok());
  EXPECT_EQ(reloaded.Current()->version, 5);
  EXPECT_EQ(reloaded.Current()->myid, kC);
  const std::optional<Endpoint> boot = reloaded.BootMaster();
  ASSERT_TRUE(boot.has_value());
  EXPECT_EQ(boot->host, "127.0.0.1");
  EXPECT_EQ(boot->port, 7000);
}

TEST_F(ClusterManagerTest, ReadersOnOtherThreadsSeeEveryPublishInOrder) {  // D3
  auto m = Make();
  std::atomic<bool> stop{false};
  std::atomic<bool> monotonic{true};
  std::thread reader([&] {
    int64_t last = kNoVersion;
    while (!stop.load()) {
      const int64_t v = m->Current()->version;
      if (v < last) monotonic = false;
      last = v;
    }
  });
  for (int v = 1; v <= 50; ++v) ASSERT_TRUE(m->SetNodes(kNodes, v, false).ok());
  stop = true;
  reader.join();
  EXPECT_TRUE(monotonic.load());
  EXPECT_EQ(m->Current()->version, 50);
}

}  // namespace
}  // namespace tideskv::cluster
