#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/crc16.h"
#include "multinode/cluster_client.h"
#include "multinode/cluster_mixed_workload.h"
#include "multinode/multinode_helpers.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

// L5 for M7b (D25): a slot migration under load, driven as kvrocks-controller drives it, and the node kill,
// failover and push race that can interrupt one. Shard 0 = {0 master, 1 replica}, shard 1 = {2 master,
// 3 replica}; 1000-1999 moves from shard 0 to shard 1.
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

const std::vector<NodeLine> kTwoShards = {{0, -1, "0-8191"}, {1, 0, ""}, {2, -1, "8192-16383"}, {3, 2, ""}};
const std::vector<NodeLine> kMoved = {
    {0, -1, "0-999 2000-8191"}, {1, 0, ""}, {2, -1, "1000-1999 8192-16383"}, {3, 2, ""}};
constexpr uint16_t kFirst = 1000;
constexpr uint16_t kLast = 1999;
const std::string kRange = "1000-1999";
const std::string kZeros(40, '0');

std::string Info(RespClient& c) { return c.Command({"CLUSTER", "INFO"}).str; }

bool WaitMigrate(RespClient& c, const std::string& range, const std::string& state,
                 std::chrono::milliseconds timeout) {
  return WaitFor(
      [&] {
        const std::string i = Info(c);
        return i.find("migrating_slot(s): " + range + "\r\n") != std::string::npos &&
               i.find("migrating_state: " + state + "\r\n") != std::string::npos;
      },
      timeout);
}

std::string DigestSlots(RespClient& c, const std::string& range) {
  return c.Command({"DEBUG", "DIGEST-SLOTS", range, "TTL"}).str;
}

void Start(Nodes& n) {
  ASSERT_NO_FATAL_FAILURE(PushTopology(n.ptrs, NodesString(n.ptrs, kTwoShards), 1));
  ASSERT_TRUE(WaitConnected(*n.ptrs[1]));
  ASSERT_TRUE(WaitConnected(*n.ptrs[3]));
}

void Config(RespClient& c, const std::vector<std::pair<std::string, std::string>>& knobs) {
  for (const auto& [k, v] : knobs) ASSERT_EQ(c.Command({"CONFIG", "SET", k, v}).str, "OK") << k;
}

void Fill(RespClient& c, const std::string& tag, int count) {  // 512-byte elements
  const std::string value(512, 'f');
  for (int i = 0; i < count; ++i)
    ASSERT_EQ(c.Command({"RPUSH", "{" + tag + "}filler", value}).integer, i + 1);
}

// A tag in the range whose slot holds no key on `c` yet (the ack load lands in some of the range's slots), so
// a count of 1 there means the filler's meta has arrived.
std::string EmptySlotTag(RespClient& c) {
  for (const std::string& t : TagsInRange(kFirst, kLast, 64)) {
    if (c.Command({"CLUSTER", "COUNTKEYSINSLOT", std::to_string(KeySlot(t))}).integer == 0) return t;
  }
  ADD_FAILURE() << "every candidate slot holds a key";
  return "t0";
}

// The range is gone from `s`: its digest is empty and the tags' slots count no key (D9).
void ExpectRangeGone(const TestServer& s, const std::vector<std::string>& tags) {
  auto c = Client(s);
  EXPECT_EQ(DigestSlots(*c, kRange), kZeros) << "port " << s.port();
  std::set<uint16_t> slots;
  for (const std::string& t : tags) slots.insert(KeySlot(t));
  for (uint16_t slot : slots) {
    EXPECT_EQ(c->Command({"CLUSTER", "COUNTKEYSINSLOT", std::to_string(slot)}).integer, 0) << slot;
  }
}

void ExpectNoMissingExpireEntry(const Nodes& n) {
  for (const TestServer* s : n.ptrs) {
    auto c = Client(*s);
    const RespValue d = DigestIndex(*c);
    ASSERT_EQ(d.elements.size(), 4u) << ToDebugString(d);
    EXPECT_EQ(d.elements[2].integer, 0) << "port " << s->port() << " lost expire entries";
  }
}

// The M7b exit criterion: a mixed-type load on the migrating range and a SET load on every slot, through a
// migration the test drives as the controller does. No acknowledged write is lost, the moved range is equal
// on both shards (TTLs included), the source and its replica end up empty for it, and both shards converge.
TEST(Migration, UnderLoadLosesNoAcknowledgedWrite) {
  const uint32_t seed = Seed();
  Nodes n(4);
  ASSERT_NO_FATAL_FAILURE(Start(n));
  auto src = Client(*n.ptrs[0]);
  auto dst = Client(*n.ptrs[2]);
  const std::vector<std::string> tags = TagsInRange(kFirst, kLast, 16);
  ASSERT_NO_FATAL_FAILURE(Fill(*src, tags[0], 4000));  // ~2 MB: the snapshot takes ~2 s at 1 MiB/s
  ASSERT_NO_FATAL_FAILURE(Config(*src, {{"migrate-batch-rate-limit-mb", "1"},
                                        {"migrate-batch-size-kb", "4"},
                                        {"migrate-delay-ms", "500"},
                                        {"migrate-sequence-gap", "1"}}));  // a catch-up round always runs
  ClusterWorkload acks(n.Ports(), 4);
  ClusterMixedWorkload mixed(n.Ports(), tags, 4, seed);
  acks.Start();
  mixed.Start();
  std::this_thread::sleep_for(500ms);
  ASSERT_EQ(src->Command({"CLUSTERX", "MIGRATE", kRange, NodeId(2)}).str, "OK");
  std::this_thread::sleep_for(1s);  // inside the snapshot: these writes land in it or in the catch-up
  mixed.Stop();
  acks.Stop();
  const std::string probe = "{" + tags[1] + "}probe";
  ASSERT_EQ(src->Command({"SET", probe, "x"}).str, "OK")
      << "the loads must stop before the fence (more filler)";
  ASSERT_TRUE(WaitMigrate(*src, kRange, "success", 60s));
  EXPECT_EQ(DigestSlots(*src, kRange), DigestSlots(*dst, kRange))
      << "the fenced source and the target differ";
  ASSERT_NO_FATAL_FAILURE(PushTopology(n.ptrs, NodesString(n.ptrs, kMoved), 2));  // the controller's move
  acks.Start();  // stale slot maps: writes to the range follow MOVED to the new owner
  std::this_thread::sleep_for(1s);
  acks.Stop();
  EXPECT_EQ(acks.failures(), 0u);
  EXPECT_EQ(mixed.failures(), 0u) << (mixed.errors().empty() ? "" : mixed.errors().begin()->first);
  EXPECT_GT(mixed.steps(), 100u);
  EXPECT_GT(acks.redirects(), 0u);
  ExpectAllAcked(n.Ports(), acks.acked());
  ExpectConverged(*n.ptrs[0], {n.ptrs[1]});
  ExpectConverged(*n.ptrs[2], {n.ptrs[3]});
  ExpectRangeGone(*n.ptrs[0], tags);
  ExpectRangeGone(*n.ptrs[1], tags);
  ExpectNoMissingExpireEntry(n);
}

// D8 and D24.1's other side: the source dies after success and before the push. It boots with the success
// block, MOVED and the fence, and the push then cleans the range up on it and on its replica.
TEST(Migration, AKill9OfTheSourceAfterSuccessKeepsMovedAndThePushCleansUp) {
  Nodes n(4);
  ASSERT_NO_FATAL_FAILURE(Start(n));
  auto src = Client(*n.ptrs[0]);
  const std::vector<std::string> tags = TagsInRange(kFirst, kLast, 4);
  for (int i = 0; i < 200; ++i) {
    ASSERT_EQ(src->Command({"SET", "{" + tags[i % 4] + "}k" + std::to_string(i), std::to_string(i)}).str,
              "OK");
  }
  ASSERT_EQ(src->Command({"SET", "b", "outside"}).str, "OK");  // shard 0 keeps data outside the range
  ASSERT_EQ(src->Command({"CLUSTERX", "MIGRATE", kRange, NodeId(2), "SYNC"}).str, "OK");
  n.servers[0]->Kill9();
  n.servers[0]->Restart();
  src = Client(*n.ptrs[0]);
  EXPECT_NE(Info(*src).find("migrating_slot(s): " + kRange + "\r\ndestination_node: " + NodeId(2) +
                            "\r\nmigrating_state: success\r\n"),
            std::string::npos);
  const std::string key0 = "{" + tags[0] + "}k0";
  EXPECT_EQ(src->Command({"GET", key0}).str,
            "MOVED " + std::to_string(KeySlot(key0)) + " 127.0.0.1:" + std::to_string(n.ptrs[2]->port()));
  ASSERT_TRUE(WaitConnected(*n.ptrs[1], 60s));
  ASSERT_NO_FATAL_FAILURE(PushTopology(n.ptrs, NodesString(n.ptrs, kMoved), 2));
  ClusterClient reader(n.Ports());
  for (int i = 0; i < 200; ++i) {
    const std::string key = "{" + tags[i % 4] + "}k" + std::to_string(i);
    EXPECT_EQ(reader.Command({"GET", key}, key).str, std::to_string(i)) << key;
  }
  ExpectConverged(*n.ptrs[0], {n.ptrs[1]});
  ExpectConverged(*n.ptrs[2], {n.ptrs[3]});
  ExpectRangeGone(*n.ptrs[0], tags);
  ExpectRangeGone(*n.ptrs[1], tags);
}

// A controller failover of the target shard mid-import: the old target master is demoted (its next IMPORTDATA
// answers READONLY, so the job fails), and the promoted replica erases the half import it replicated (D8).
TEST(Migration, AFailoverOfTheTargetMidImportErasesTheHalfImport) {
  Nodes n(4);
  ASSERT_NO_FATAL_FAILURE(Start(n));
  ClusterWorkload acks(n.Ports(), 2);  // data in both shards, so both masters' digests are non-empty
  acks.Start();
  std::this_thread::sleep_for(1s);
  acks.Stop();
  auto src = Client(*n.ptrs[0]);
  const std::vector<std::string> tags = {EmptySlotTag(*src)};
  ASSERT_NO_FATAL_FAILURE(Fill(*src, tags[0], 4000));
  ASSERT_NO_FATAL_FAILURE(
      Config(*src, {{"migrate-batch-rate-limit-mb", "1"}, {"migrate-batch-size-kb", "1"}}));
  ASSERT_EQ(src->Command({"CLUSTERX", "MIGRATE", kRange, NodeId(2)}).str, "OK");
  auto target_replica = Client(*n.ptrs[3]);
  const std::string slot = std::to_string(KeySlot(tags[0]));
  ASSERT_TRUE(WaitFor(
      [&] { return target_replica->Command({"CLUSTER", "COUNTKEYSINSLOT", slot}).integer == 1; }, 30s))
      << "the import replicates (D21)";
  const std::vector<NodeLine> swapped = {{0, -1, "0-8191"}, {1, 0, ""}, {3, -1, "8192-16383"}, {2, 3, ""}};
  ASSERT_NO_FATAL_FAILURE(PushTopology(n.ptrs, NodesString(n.ptrs, swapped), 2));
  ASSERT_TRUE(WaitMigrate(*src, kRange, "fail", 60s));
  ASSERT_TRUE(WaitFor(
      [&] { return Info(*target_replica).find("import_state: error\r\n") != std::string::npos; }, 30s));
  // The restore publishes import_state: error, then erases the half import on its worker (D8): wait for it.
  std::string digest;
  EXPECT_TRUE(WaitFor([&] { return (digest = DigestSlots(*target_replica, kRange)) == kZeros; }, 30s))
      << digest;
  ASSERT_TRUE(WaitConnected(*n.ptrs[2], 60s));
  ExpectConverged(*n.ptrs[3], {n.ptrs[2]});
  EXPECT_EQ(DigestSlots(*Client(*n.ptrs[2]), kRange), kZeros);
  EXPECT_EQ(src->Command({"LLEN", "{" + tags[0] + "}filler"}).integer, 4000)
      << "the source still owns its data";
  ExpectAllAcked(n.Ports(), acks.acked());
}

// Review Focus #3 across real nodes: the controller pushes v+1 right after MIGRATE's +OK, a one-slot range
// succeeds before or after it (a repeated push at v+2 lands after it), and v+3 moves the range: routing never
// reverts and the source is cleaned up.
TEST(Migration, TheControllerRaceKeepsRoutingAndCleansUp) {
  Nodes n(4);
  ASSERT_NO_FATAL_FAILURE(Start(n));
  auto src = Client(*n.ptrs[0]);
  auto dst = Client(*n.ptrs[2]);
  const std::string tag = TagsInRange(100, 100, 1)[0];
  for (int i = 0; i < 10; ++i) {
    ASSERT_EQ(src->Command({"SET", "{" + tag + "}" + std::to_string(i), std::to_string(i)}).str, "OK");
  }
  ASSERT_EQ(src->Command({"SET", "b", "outside"}).str, "OK");
  ASSERT_EQ(src->Command({"CLUSTERX", "MIGRATE", "100", NodeId(2)}).str, "OK");
  ASSERT_NO_FATAL_FAILURE(PushTopology(n.ptrs, NodesString(n.ptrs, kTwoShards), 2));  // the controller's v+1
  ASSERT_TRUE(WaitMigrate(*src, "100", "success", 30s));
  const std::string key0 = "{" + tag + "}0";
  const std::string moved = "MOVED 100 127.0.0.1:" + std::to_string(n.ptrs[2]->port());
  ClusterClient reader(n.Ports());
  const auto expect_routed = [&] {
    EXPECT_EQ(src->Command({"GET", key0}).str, moved);
    EXPECT_EQ(src->Command({"SET", key0, "x"}).str, moved);
    EXPECT_EQ(dst->Command({"GET", key0}).str, "0") << "imported: served before the move";
    for (int i = 0; i < 10; ++i) {
      const std::string key = "{" + tag + "}" + std::to_string(i);
      EXPECT_EQ(reader.Command({"GET", key}, key).str, std::to_string(i));
    }
  };
  expect_routed();
  // The v+1 above usually lands while the job runs; a controller that pushes the unchanged map again lands
  // after the success whichever order that one took, and must not revert routing either (D7).
  ASSERT_NO_FATAL_FAILURE(PushTopology(n.ptrs, NodesString(n.ptrs, kTwoShards), 3));
  expect_routed();
  const std::vector<NodeLine> moved100 = {
      {0, -1, "0-99 101-8191"}, {1, 0, ""}, {2, -1, "100 8192-16383"}, {3, 2, ""}};
  ASSERT_NO_FATAL_FAILURE(PushTopology(n.ptrs, NodesString(n.ptrs, moved100), 4));
  EXPECT_EQ(src->Command({"CLUSTER", "COUNTKEYSINSLOT", "100"}).integer, 0);
  auto src_replica = Client(*n.ptrs[1]);
  EXPECT_TRUE(
      WaitFor([&] { return src_replica->Command({"CLUSTER", "COUNTKEYSINSLOT", "100"}).integer == 0; }, 30s));
  EXPECT_NE(Info(*src).find("migrating_slot(s): 100\r\n"), std::string::npos);  // the block stays (D15)
  for (int i = 0; i < 10; ++i) {
    const std::string key = "{" + tag + "}" + std::to_string(i);
    EXPECT_EQ(reader.Command({"GET", key}, key).str, std::to_string(i));
  }
  ExpectConverged(*n.ptrs[0], {n.ptrs[1]});
  ExpectConverged(*n.ptrs[2], {n.ptrs[3]});
}

}  // namespace
}  // namespace tideskv::test
