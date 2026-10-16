#include "cluster/cluster_views.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "cluster/nodes_parser.h"

namespace tideskv::cluster {
namespace {

const std::string kA(40, 'a');
const std::string kB(40, 'b');
const std::string kC(40, 'c');
const std::string kD(40, 'd');

Topology Make(const std::string& nodes, int64_t version, const std::string& myid) {
  auto p = ParseNodes(nodes);
  EXPECT_TRUE(p.ok()) << p.status().ToString();
  Topology t;
  t.version = version;
  t.nodes = p->nodes;
  t.owner = p->owner;
  t.myid = myid;
  t.ResolveMyself();
  return t;
}

const std::string kFour = kA + " 10.0.0.1 7000 master - 0-8191\n" + kB +
                          " 10.0.0.2 7001 master - 8192-16000\n" + kC + " 10.0.0.3 7002 slave " + kA + "\n" +
                          kD + " 10.0.0.4 7003 slave,fail " + kA + "\n";

TEST(ClusterViews, InfoIsKvrocksNineCrlfLines) {  // cluster.cc:406-453
  EXPECT_EQ(ClusterInfoText(Make(kFour, 3, kA)),
            "cluster_state:ok\r\ncluster_slots_assigned:16001\r\ncluster_slots_ok:16001\r\n"
            "cluster_slots_pfail:0\r\ncluster_slots_fail:0\r\ncluster_known_nodes:4\r\ncluster_size:2\r\n"
            "cluster_current_epoch:3\r\ncluster_my_epoch:3\r\n");
}

TEST(ClusterViews, NodesLinesAreKvrocksInStringOrder) {  // cluster.cc:563-612
  EXPECT_EQ(ClusterNodesText(Make(kFour, 3, kC), 1000),
            kA + " 10.0.0.1:7000@17000 master - 999 1000 3 connected 0-8191\n" + kB +
                " 10.0.0.2:7001@17001 master - 999 1000 3 connected 8192-16000\n" + kC +
                " 10.0.0.3:7002@17002 myself,slave " + kA + " 999 1000 3 connected\n" + kD +
                " 10.0.0.4:7003@17003 slave,fail " + kA + " 999 1000 3 disconnected\n");
}

TEST(ClusterViews, ReplicasListsTheMastersSlavesOrRefusesLikeKvrocks) {  // cluster.cc:516-556
  const Topology t = Make(kFour, 3, kA);
  auto r = ClusterReplicasText(t, kA, 1000);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(*r, kC + " 10.0.0.3:7002@17002 slave " + kA + " 999 1000 3 connected\n" + kD +
                    " 10.0.0.4:7003@17003 slave,fail " + kA + " 999 1000 3 disconnected\n");
  EXPECT_EQ(*ClusterReplicasText(t, kB, 1000), "");
  EXPECT_EQ(ClusterReplicasText(t, kC, 1000).status().message(), "ERR The node isn't a master");
  EXPECT_EQ(ClusterReplicasText(t, "unknown", 1000).status().message(), "ERR Invalid cluster node id");
}

TEST(ClusterViews, SlotRunsListTheOwnerThenItsHealthyReplicas) {  // D12
  const Topology t = Make(kFour, 3, kA);                          // the runs point into it
  const auto runs = SlotRuns(t);
  ASSERT_EQ(runs.size(), 2u);
  EXPECT_EQ(runs[0].start, 0);
  EXPECT_EQ(runs[0].end, 8191);
  ASSERT_EQ(runs[0].nodes.size(), 2u);  // A and C; D is failed
  EXPECT_EQ(runs[0].nodes[1]->id, kC);
  EXPECT_EQ(runs[1].end, 16000);
  ASSERT_EQ(runs[1].nodes.size(), 1u);
}

TEST(ClusterViews, ShardsAreMastersWithAllTheirReplicas) {
  const Topology t = Make(kFour, 3, kA);  // the shards point into it
  const auto shards = Shards(t);
  ASSERT_EQ(shards.size(), 2u);
  EXPECT_EQ(shards[0].master->id, kA);
  EXPECT_EQ(shards[0].replicas.size(), 2u);
  EXPECT_EQ(shards[1].slots, (std::vector<SlotRange>{{8192, 16000}}));
}

// kvrocks-controller v1.3.0, transcribed: store/cluster_node.go:176-208 (CLUSTER INFO) and
// store/cluster.go:253-321 (CLUSTER NODES). Whatever they reject would make the controller re-push forever
// (wrong epoch) or fail an import.
int64_t ControllerEpoch(const std::string& info) {
  int64_t epoch = -1;
  size_t pos = 0;
  while (pos < info.size()) {
    size_t end = info.find("\r\n", pos);
    if (end == std::string::npos) end = info.size();
    const std::string line = info.substr(pos, end - pos);
    pos = end + 2;
    if (std::count(line.begin(), line.end(), ':') != 1) continue;
    const size_t colon = line.find(':');
    if (line.substr(0, colon) == "cluster_current_epoch") epoch = std::stoll(line.substr(colon + 1));
  }
  return epoch;
}

bool ControllerSlotRange(const std::string& s) {  // store/slot.go ParseSlotRange: "N" or "a-b", a <= b
  const size_t dash = s.find('-');
  try {
    if (dash == std::string::npos)
      return std::stoi(s) >= 0 && std::stoi(s) <= 16383 && std::to_string(std::stoi(s)) == s;
    if (s.find('-', dash + 1) != std::string::npos || dash == 0) return false;
    const int a = std::stoi(s.substr(0, dash));
    const int b = std::stoi(s.substr(dash + 1));
    return a >= 0 && a <= b && b <= 16383;
  } catch (...) {
    return false;
  }
}

std::vector<std::string> SplitExact(const std::string& s, char sep) {  // Go strings.Split: keeps empties
  std::vector<std::string> out;
  size_t pos = 0;
  for (;;) {
    const size_t at = s.find(sep, pos);
    out.push_back(s.substr(pos, at == std::string::npos ? std::string::npos : at - pos));
    if (at == std::string::npos) return out;
    pos = at + 1;
  }
}

// Review Focus #5.
TEST(ClusterViews, NodesAndInfoParseWithTheControllersRules) {
  const std::string nodes = kFour + std::string(40, 'e') + " 10.0.0.5 7004 master -\n";  // an empty shard
  const Topology t = Make(nodes, 42, kA);
  const std::string info = ClusterInfoText(t);
  EXPECT_EQ(ControllerEpoch(info), 42);
  EXPECT_EQ(info.find("\n"), info.find("\r\n") + 1);  // no bare LF
  std::string text = ClusterNodesText(t, 5000);
  ASSERT_EQ(text.back(), '\n');
  text.pop_back();  // the controller's TrimRight(s, "\n")
  const auto lines = SplitExact(text, '\n');
  ASSERT_EQ(lines.size(), t.nodes.size());
  for (const std::string& line : lines) {
    const auto f = SplitExact(line, ' ');
    ASSERT_GE(f.size(), 7u) << line;
    for (const std::string& field : f)
      EXPECT_FALSE(field.empty()) << "double or trailing space: '" << line << "'";
    EXPECT_EQ(f[6], "42") << line;
    const std::string role = f[2].find(',') == std::string::npos ? f[2] : SplitExact(f[2], ',')[1];  // v1.3.0
    if (role == "master") {
      for (size_t i = 8; i < f.size(); ++i) EXPECT_TRUE(ControllerSlotRange(f[i])) << f[i];
    }
  }
}

}  // namespace
}  // namespace tideskv::cluster
