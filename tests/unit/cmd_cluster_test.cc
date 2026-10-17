#include <gtest/gtest.h>

#include <string>

#include "cluster/slot_states.h"
#include "support/cluster_harness.h"

namespace tideskv::test {
namespace {

TEST(ClusterCmd, DisabledModeAnswersRedisText) {  // cluster.c:5995
  CommandHarness h;
  EXPECT_EQ(h.Run({"CLUSTER", "INFO"}), Err("ERR This instance has cluster support disabled"));
  EXPECT_EQ(h.Run({"CLUSTER", "KEYSLOT", "foo"}), Err("ERR This instance has cluster support disabled"));
}

TEST(ClusterCmd, BeforeAnyTopologyOnlyLocalSubcommandsWork) {  // D12
  CommandHarness h;
  ClusterHarness c(h);
  for (const char* sub : {"INFO", "NODES", "SLOTS", "SHARDS"}) {
    EXPECT_EQ(h.Run({"CLUSTER", sub}), Err("CLUSTERDOWN The cluster is not initialized")) << sub;
  }
  EXPECT_EQ(h.Run({"CLUSTER", "REPLICAS", kNodeA}), Err("CLUSTERDOWN The cluster is not initialized"));
  EXPECT_EQ(h.Run({"CLUSTER", "KEYSLOT", "foo"}), Int(12182));
  EXPECT_EQ(h.Run({"CLUSTER", "MYID"}), Bulk(""));
  EXPECT_EQ(h.Run({"CLUSTER", "COUNTKEYSINSLOT", "0"}), Int(0));
}

TEST(ClusterCmd, SubcommandAndArgumentErrors) {  // D18, cluster.c:6341-6375
  CommandHarness h;
  ClusterHarness c(h);
  EXPECT_EQ(h.Run({"CLUSTER", "NOPE"}),
            Err("ERR unknown subcommand or wrong number of arguments for 'NOPE'. Try CLUSTER HELP."));
  EXPECT_EQ(h.Run({"CLUSTER", "nodes", "x"}),
            Err("ERR unknown subcommand or wrong number of arguments for 'nodes'. Try CLUSTER HELP."));
  EXPECT_EQ(h.Run({"CLUSTER", "RESET", "x"}), Err("ERR syntax error"));
  EXPECT_EQ(h.Run({"CLUSTER", "COUNTKEYSINSLOT", "x"}), Err("ERR value is not an integer or out of range"));
  EXPECT_EQ(h.Run({"CLUSTER", "COUNTKEYSINSLOT", "16384"}), Err("ERR Invalid slot"));
  EXPECT_EQ(h.Run({"CLUSTER", "GETKEYSINSLOT", "0", "-1"}), Err("ERR Invalid slot or number of keys"));
  EXPECT_EQ(h.Run({"CLUSTER", "GETKEYSINSLOT", "x", "1"}),
            Err("ERR value is not an integer or out of range"));
}

TEST(ClusterCmd, InfoAndNodesAreBulkInResp2AndVerbatimInResp3) {
  CommandHarness h;
  ClusterHarness c(h);
  c.Push(kThreeNodes, 4);
  const std::string info = h.Run({"CLUSTER", "INFO"});
  EXPECT_EQ(info.rfind("$", 0), 0u);
  EXPECT_NE(info.find("cluster_current_epoch:4\r\n"), std::string::npos);
  h.Resp3();
  EXPECT_EQ(h.Run({"CLUSTER", "NODES"}).rfind("=", 0), 0u);
  EXPECT_NE(h.Run({"CLUSTER", "INFO"}).find("txt:cluster_state:ok\r\n"), std::string::npos);
}

TEST(ClusterCmd, SlotsUseKvrocksThreeElementNodes) {  // D12, cmd_cluster.cc:76-94
  CommandHarness h;
  ClusterHarness c(h);
  c.Push(kThreeNodes, 1);
  const auto node = [](uint16_t port, const std::string& id) {
    return Arr({Bulk("127.0.0.1"), Int(port), Bulk(id)});
  };
  EXPECT_EQ(h.Run({"CLUSTER", "SLOTS"}),
            Arr({Arr({Int(0), Int(8191), node(7000, kNodeA), node(7002, kNodeC)}),
                 Arr({Int(8192), Int(16383), node(7001, kNodeB)})}));
}

TEST(ClusterCmd, ShardsAreRedisMapsFlatInResp2) {  // redis cluster.c:5771-5876
  CommandHarness h;
  ClusterHarness c(h);
  c.Push(kThreeNodes, 1);
  const std::string resp2 = h.Run({"CLUSTER", "SHARDS"});
  EXPECT_EQ(
      resp2.rfind("*2\r\n*4\r\n$5\r\nslots\r\n*2\r\n:0\r\n:8191\r\n$5\r\nnodes\r\n*2\r\n*14\r\n$2\r\nid\r\n",
                  0),
      0u)
      << resp2;
  EXPECT_NE(resp2.find("$4\r\nrole\r\n$7\r\nreplica\r\n"), std::string::npos);
  EXPECT_NE(resp2.find("$6\r\nhealth\r\n$6\r\nonline\r\n"), std::string::npos);
  h.Resp3();
  EXPECT_EQ(h.Run({"CLUSTER", "SHARDS"}).rfind("*2\r\n%2\r\n$5\r\nslots\r\n", 0), 0u);
}

TEST(ClusterCmd, CountAndGetKeysInSlotReadThisNodesDbZero) {
  CommandHarness h;
  ClusterHarness c(h);
  c.Push(kThreeNodes, 1);
  ASSERT_EQ(h.Run({"SET", "b", "1"}), kOk);
  ASSERT_EQ(h.Run({"SET", "{b}x", "1"}), kOk);
  EXPECT_EQ(h.Run({"CLUSTER", "COUNTKEYSINSLOT", "3300"}), Int(2));
  EXPECT_EQ(h.Run({"CLUSTER", "GETKEYSINSLOT", "3300", "1"}), Arr({Bulk("b")}));
  EXPECT_EQ(h.Run({"CLUSTER", "GETKEYSINSLOT", "3300", "10"}), Arr({Bulk("b"), Bulk("{b}x")}));
  EXPECT_EQ(h.Run({"CLUSTER", "GETKEYSINSLOT", "3300", "0"}), "*0\r\n");
}

TEST(ClusterCmd, ResetNeedsAnEmptyKeyspace) {  // cluster.cc:960-1000
  CommandHarness h;
  ClusterHarness c(h);
  c.Push(kThreeNodes, 1);
  ASSERT_EQ(h.Run({"SET", "b", "1"}), kOk);
  EXPECT_EQ(h.Run({"CLUSTER", "RESET", "HARD"}), Err("ERR Can't reset cluster while database is not empty"));
  ASSERT_EQ(h.Run({"DEL", "b"}), Int(1));
  EXPECT_EQ(h.Run({"CLUSTER", "RESET", "soft"}), kOk);
  EXPECT_EQ(h.Run({"CLUSTERX", "VERSION"}), Bulk("-1"));
}

TEST(ClusterCmd, ReplicasAndHelp) {
  CommandHarness h;
  ClusterHarness c(h);
  c.Push(kThreeNodes, 1);
  EXPECT_NE(h.Run({"CLUSTER", "REPLICAS", kNodeA}).find(kNodeC + " 127.0.0.1:7002@17002 slave " + kNodeA),
            std::string::npos);
  EXPECT_EQ(h.Run({"CLUSTER", "REPLICAS", kNodeC}), Err("ERR The node isn't a master"));
  EXPECT_NE(
      h.Run({"CLUSTER", "HELP"}).find("CLUSTER <subcommand> [<arg> [value] [opt] ...]. Subcommands are:"),
      std::string::npos);
}

TEST(ClusterCmd, InfoShowsTheMigrationBlocksOfThePublishedStates) {  // D15
  CommandHarness h;
  ClusterHarness c(h);
  c.Push(kThreeNodes, 1);
  auto s = std::make_shared<cluster::SlotStates>();
  s->job = cluster::SourceJob{{3300, 3300}, kNodeB, "127.0.0.1", 7001, cluster::MigrateState::kStart};
  c.manager->PublishStates(s);
  const std::string info = h.Run({"CLUSTER", "INFO"});
  EXPECT_NE(info.find("cluster_my_epoch:1\r\nmigrating_slot(s): 3300\r\ndestination_node: " + kNodeB +
                      "\r\nmigrating_state: start\r\n"),
            std::string::npos)
      << info;
  h.services.replication_.set_role(ReplRole::kReplica);  // replicating: no block (cluster.cc:440)
  EXPECT_EQ(h.Run({"CLUSTER", "INFO"}).find("migrating_slot"), std::string::npos);
}

}  // namespace
}  // namespace tideskv::test
