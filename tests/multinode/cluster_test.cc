#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "multinode/cluster_client.h"
#include "multinode/multinode_helpers.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

// L5 for M7a: topologies pushed as kvrocks-controller pushes them, a MOVED-following load, failovers by
// topology push (planned, and with CLIENT PAUSE like the controller main's manual failover), restarts.
namespace tideskv::test {
namespace {

using namespace multinode;

struct Nodes {
  explicit Nodes(int n) {
    for (int i = 0; i < n; ++i) {
      servers.push_back(std::make_unique<TestServer>(
          TIDESKV_SERVER_BIN, TestServer::Options{.extra_args = {"--cluster-enabled", "yes"}}));
      ptrs.push_back(servers.back().get());
    }
  }
  std::vector<uint16_t> Ports() const {
    std::vector<uint16_t> out;
    out.reserve(ptrs.size());
    for (const TestServer* s : ptrs) out.push_back(s->port());
    return out;
  }
  std::vector<std::unique_ptr<TestServer>> servers;
  std::vector<const TestServer*> ptrs;
};

// Shard 0 = {0 master, 1 replica}, shard 1 = {2 master, 3 replica}.
const std::vector<NodeLine> kTwoShards = {{0, -1, "0-8191"}, {1, 0, ""}, {2, -1, "8192-16383"}, {3, 2, ""}};

// Minimum acknowledged writes per load phase, so the zero-loss checks never pass on an empty set (a sanitizer
// build does several hundred per second).
constexpr size_t kMinAcked = 100;
constexpr size_t kMinAckedAfterSwitch = 50;

void RunFor(ClusterWorkload& load, std::chrono::milliseconds for_how_long) {
  load.Start();
  std::this_thread::sleep_for(for_how_long);
  load.Stop();
}

// The master's sequence equals the replica's and stays so for 200 ms (no straggler commit).
bool CaughtUpAndQuiet(const TestServer& master, const TestServer& replica) {
  auto m = Client(master);
  auto r = Client(replica);
  return WaitFor(
      [&] {
        const int64_t seq = Sequence(*m);
        if (Sequence(*r) != seq) return false;
        std::this_thread::sleep_for(200ms);
        return Sequence(*m) == seq && Sequence(*r) == seq;
      },
      20s);
}

TEST(Cluster, TopologyPushRoutesAMovedFollowingLoadAndReplicates) {
  Nodes n(4);
  ASSERT_NO_FATAL_FAILURE(PushTopology(n.ptrs, NodesString(n.ptrs, kTwoShards), 1));
  ASSERT_TRUE(WaitConnected(*n.ptrs[1]));
  ASSERT_TRUE(WaitConnected(*n.ptrs[3]));
  ClusterWorkload load({n.ptrs[1]->port()}, 4);  // seeded with a replica only: the map comes from SLOTS
  RunFor(load, 3s);
  EXPECT_GT(load.acked().size(), kMinAcked);
  EXPECT_EQ(load.failures(), 0u);
  ExpectConverged(*n.ptrs[0], {n.ptrs[1]});
  ExpectConverged(*n.ptrs[2], {n.ptrs[3]});
  ExpectAllAcked(n.Ports(), load.acked());
  for (const TestServer* s : n.ptrs) {
    const std::string nodes = Client(*s)->Command({"CLUSTER", "NODES"}).str;
    EXPECT_EQ(std::count(nodes.begin(), nodes.end(), '\n'), 4) << nodes;
  }
  EXPECT_EQ(Client(*n.ptrs[0])->Command({"MSET", "ack:1", "x", "ack:2", "y"}).str,
            "CROSSSLOT Keys in request don't hash to the same slot");
}

TEST(Cluster, PlannedFailoverByTopologyPushConvergesAndClientsFollowMoved) {
  const uint32_t seed = Seed();
  std::mt19937 rng(seed);
  Nodes n(4);
  ASSERT_NO_FATAL_FAILURE(PushTopology(n.ptrs, NodesString(n.ptrs, kTwoShards), 1));
  ASSERT_TRUE(WaitConnected(*n.ptrs[1]));
  ASSERT_TRUE(WaitConnected(*n.ptrs[3]));
  ClusterWorkload load(n.Ports(), 4);
  RunFor(load, 2s);
  const size_t before_switch = load.acked().size();
  EXPECT_GT(before_switch, kMinAcked);
  ASSERT_TRUE(CaughtUpAndQuiet(*n.ptrs[0], *n.ptrs[1]));
  // The load is stopped across the switch: writes in flight during it are the manual-failover test's job.
  const std::vector<NodeLine> swapped = {{1, -1, "0-8191"}, {0, 1, ""}, {2, -1, "8192-16383"}, {3, 2, ""}};
  std::vector<size_t> order{0, 1, 2, 3};
  std::shuffle(order.begin(), order.end(), rng);  // the controller pushes to every node in parallel
  ASSERT_NO_FATAL_FAILURE(PushTopology(n.ptrs, NodesString(n.ptrs, swapped), 2, order));
  EXPECT_EQ(LinkState(*Client(*n.ptrs[1])), "");  // a master now
  ASSERT_TRUE(WaitConnected(*n.ptrs[0], 60s));    // D19: a refused PSYNC retries within 2 s
  RunFor(load, 2s);
  EXPECT_GT(load.acked().size(), before_switch + kMinAckedAfterSwitch);
  EXPECT_EQ(load.failures(), 0u);
  EXPECT_GT(load.redirects(), 0u);  // stale maps sent shard 0's writes to node 0
  ExpectConverged(*n.ptrs[1], {n.ptrs[0]});
  ExpectConverged(*n.ptrs[2], {n.ptrs[3]});
  ExpectAllAcked(n.Ports(), load.acked());
}

// Review Focus #2: the controller main's manual failover (CLIENT PAUSE WRITE, wait for the offsets, push to
// the new then the old master, CLIENT UNPAUSE) under a running load loses no acknowledged write.
TEST(Cluster, ManualFailoverWithClientPauseLosesNoAcknowledgedWrite) {
  Nodes n(2);
  ASSERT_NO_FATAL_FAILURE(PushTopology(n.ptrs, NodesString(n.ptrs, {{0, -1, "0-16383"}, {1, 0, ""}}), 1));
  ASSERT_TRUE(WaitConnected(*n.ptrs[1]));
  ClusterWorkload load(n.Ports(), 4);
  load.Start();
  std::this_thread::sleep_for(1s);
  auto old_master = Client(*n.ptrs[0]);
  ASSERT_EQ(old_master->Command({"CLIENT", "PAUSE", "10000", "WRITE"}).str, "OK");
  const size_t at_pause = load.acked().size();
  EXPECT_GT(at_pause, kMinAcked);
  ASSERT_TRUE(CaughtUpAndQuiet(*n.ptrs[0], *n.ptrs[1]));
  ASSERT_NO_FATAL_FAILURE(
      PushTopology(n.ptrs, NodesString(n.ptrs, {{1, -1, "0-16383"}, {0, 1, ""}}), 2, {1, 0}));
  ASSERT_EQ(old_master->Command({"CLIENT", "UNPAUSE"}).str, "OK");
  // The load goes on against the new master: writes acknowledged after the switch are checked too.
  EXPECT_TRUE(WaitFor([&] { return load.acked().size() > at_pause + kMinAckedAfterSwitch; }, 30s))
      << "acknowledged at the pause " << at_pause << ", now " << load.acked().size();
  load.Stop();
  EXPECT_EQ(load.failures(), 0u);
  EXPECT_GT(load.redirects(), 0u);  // the paused writes were re-dispatched and MOVED
  ASSERT_TRUE(WaitConnected(*n.ptrs[0], 60s));
  ExpectConverged(*n.ptrs[1], {n.ptrs[0]});
  ExpectAllAcked(n.Ports(), load.acked());
}

TEST(Cluster, RestartReloadsTheTopologyAndAKill9KeepsAnAcknowledgedPush) {
  Nodes n(4);
  ASSERT_NO_FATAL_FAILURE(PushTopology(n.ptrs, NodesString(n.ptrs, kTwoShards), 1));
  ASSERT_TRUE(WaitConnected(*n.ptrs[1]));
  ASSERT_TRUE(WaitConnected(*n.ptrs[3]));
  ClusterWorkload load(n.Ports(), 4);
  RunFor(load, 2s);
  EXPECT_GT(load.acked().size(), kMinAcked);
  EXPECT_EQ(load.failures(), 0u);
  for (auto& s : n.servers) ASSERT_EQ(s->Terminate(), 0);
  for (auto& s : n.servers) s->Restart();
  for (const TestServer* s : n.ptrs) EXPECT_EQ(Client(*s)->Command({"CLUSTERX", "VERSION"}).str, "1");
  ASSERT_TRUE(WaitConnected(*n.ptrs[1], 60s));
  ASSERT_TRUE(WaitConnected(*n.ptrs[3], 60s));
  ExpectConverged(*n.ptrs[0], {n.ptrs[1]});
  ExpectConverged(*n.ptrs[2], {n.ptrs[3]});
  ExpectAllAcked(n.Ports(), load.acked());
  const std::vector<NodeLine> moved = {{0, -1, "0-4095"}, {1, 0, ""}, {2, -1, "4096-16383"}, {3, 2, ""}};
  ASSERT_NO_FATAL_FAILURE(PushTopology(n.ptrs, NodesString(n.ptrs, moved), 2));
  n.servers[3]->Kill9();
  n.servers[3]->Restart();
  EXPECT_EQ(Client(*n.ptrs[3])->Command({"CLUSTERX", "VERSION"}).str, "2");
  EXPECT_TRUE(WaitConnected(*n.ptrs[3], 60s));
}

}  // namespace
}  // namespace tideskv::test
