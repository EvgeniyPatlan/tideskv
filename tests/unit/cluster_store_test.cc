#include "cluster/cluster_store.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "cluster/local_addrs.h"
#include "cluster/nodes_parser.h"
#include "storage/memory_storage.h"

namespace tideskv::cluster {
namespace {

const std::string kA(40, 'a');
const std::string kB(40, 'b');

TEST(ClusterStore, AMissingRecordIsAnUninitialisedTopology) {
  InMemoryStorage s;
  auto t = LoadClusterCfg(s);
  ASSERT_TRUE(t.ok()) << t.status().ToString();
  EXPECT_EQ((*t)->version, kNoVersion);
  EXPECT_TRUE((*t)->myid.empty());
}

TEST(ClusterStore, SaveThenLoadRoundTripsVersionMyidNodesAndFail) {
  InMemoryStorage s;
  Topology t;
  t.version = 9;
  t.myid = kB;
  auto p = ParseNodes(kA + " 10.0.0.1 7000 master - 0-100 200\n" + kB + " 10.0.0.2 7001 slave,fail " + kA);
  ASSERT_TRUE(p.ok());
  t.nodes = p->nodes;
  t.owner = p->owner;
  t.ResolveMyself();
  ASSERT_TRUE(SaveClusterCfg(s, t).ok());
  auto back = LoadClusterCfg(s);
  ASSERT_TRUE(back.ok()) << back.status().ToString();
  EXPECT_EQ((*back)->version, 9);
  EXPECT_EQ((*back)->myid, kB);
  EXPECT_EQ((*back)->myself, 1);
  EXPECT_EQ((*back)->nodes, t.nodes);
  EXPECT_EQ((*back)->owner, t.owner);
  ASSERT_TRUE(DeleteClusterCfg(s).ok());
  EXPECT_EQ((*LoadClusterCfg(s))->version, kNoVersion);
}

TEST(ClusterStore, AnIdWithoutATopologyIsKept) {  // SETNODEID before the first SETNODES
  InMemoryStorage s;
  Topology t;
  t.myid = kA;
  ASSERT_TRUE(SaveClusterCfg(s, t).ok());
  auto back = LoadClusterCfg(s);
  ASSERT_TRUE(back.ok());
  EXPECT_EQ((*back)->myid, kA);
  EXPECT_EQ((*back)->myself, -1);
}

TEST(ClusterStore, ADamagedRecordIsCorruption) {
  EXPECT_EQ(DecodeClusterCfg("").status().code(), StatusCode::kCorruption);
  EXPECT_EQ(DecodeClusterCfg(std::string("\x02", 1) + std::string(9, '\0')).status().code(),
            StatusCode::kCorruption);
  std::string bad(1, '\x01');
  bad += std::string(7, '\0') + "\x05";  // version 5
  bad += std::string(1, '\0') + "not a nodes string";
  EXPECT_EQ(DecodeClusterCfg(bad).status().code(), StatusCode::kCorruption);
}

TEST(LocalAddrs, MatchesTheBindAddressOrAnyLocalAddressForAWildcard) {  // Kvrocks MatchListeningIP
  const std::vector<std::string> local = {"127.0.0.1", "10.0.0.5", "::1"};
  EXPECT_TRUE(MatchListeningIp("127.0.0.1", "127.0.0.1", local));
  EXPECT_FALSE(MatchListeningIp("127.0.0.1", "10.0.0.5", local));
  EXPECT_TRUE(MatchListeningIp("0.0.0.0", "10.0.0.5", local));
  EXPECT_TRUE(MatchListeningIp("::", "::1", local));
  EXPECT_FALSE(MatchListeningIp("0.0.0.0", "10.9.9.9", local));
  const auto real = LocalInterfaceAddresses();
  EXPECT_NE(std::find(real.begin(), real.end(), "127.0.0.1"), real.end());  // lo is always up
}

}  // namespace
}  // namespace tideskv::cluster
