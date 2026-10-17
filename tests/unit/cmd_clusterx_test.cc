#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "support/cluster_harness.h"
#include "support/migration_harness.h"

namespace tideskv::test {
namespace {

TEST(ClusterxCmd, ParsesBeforeRefusingWhenClusterModeIsOff) {  // cmd_cluster.cc:146-253
  CommandHarness h;
  EXPECT_EQ(h.Run({"CLUSTERX", "VERSION"}), Err("ERR Cluster mode is not enabled"));
  EXPECT_EQ(h.Run({"CLUSTERX", "SETNODES", "a", "-1"}), Err("ERR Cluster mode is not enabled"));
  EXPECT_EQ(h.Run({"CLUSTERX", "VERSION", "a"}),
            Err("ERR CLUSTERX command, CLUSTERX VERSION|MYID|SETNODEID|SETNODES|SETSLOT|MIGRATE|FLUSHSLOTS"));
}

TEST(ClusterxCmd, VersionAndMyIdBeforeAnyTopology) {
  CommandHarness h;
  ClusterHarness c(h);
  EXPECT_EQ(h.Run({"CLUSTERX", "VERSION"}), Bulk("-1"));
  EXPECT_EQ(h.Run({"clusterx", "myid"}), Bulk(""));
}

TEST(ClusterxCmd, SetNodeIdThenSetNodes) {
  CommandHarness h;
  ClusterHarness c(h);
  EXPECT_EQ(h.Run({"CLUSTERX", "SETNODEID", kNodeA}), kOk);
  EXPECT_EQ(h.Run({"CLUSTERX", "SETNODES", kThreeNodes, "2"}), kOk);
  EXPECT_EQ(h.Run({"CLUSTERX", "VERSION"}), Bulk("2"));
  EXPECT_EQ(h.Run({"CLUSTERX", "MYID"}), Bulk(kNodeA));
  EXPECT_EQ(h.Run({"CLUSTERX", "SETNODES", kThreeNodes, "1"}), Err("ERR Invalid cluster version"));
  EXPECT_EQ(h.Run({"CLUSTERX", "SETNODES", kThreeNodes, "1", "FORCE"}), kOk);
  EXPECT_EQ(h.Run({"CLUSTERX", "VERSION"}), Bulk("1"));
}

TEST(ClusterxCmd, ASetnodesThatMakesThisNodeASlaveDrivesReplication) {  // D10
  CommandHarness h;
  ClusterHarness c(h, 7002);
  EXPECT_EQ(h.Run({"CLUSTERX", "SETNODES", kThreeNodes, "1"}), kOk);
  EXPECT_EQ(h.services.replication_.role(), ReplRole::kReplica);
  ASSERT_EQ(h.services.replication_.become_replica.size(), 1u);
  EXPECT_EQ(h.services.replication_.become_replica[0], (std::pair<std::string, uint16_t>{"127.0.0.1", 7000}));
}

TEST(ClusterxCmd, ParseErrorsAreKvrocks) {
  CommandHarness h;
  ClusterHarness c(h);
  const std::string usage =
      "ERR CLUSTERX command, CLUSTERX VERSION|MYID|SETNODEID|SETNODES|SETSLOT|MIGRATE|FLUSHSLOTS";
  const std::vector<std::pair<ArgList, std::string>> cases = {
      {{"CLUSTERX"}, "ERR wrong number of arguments for 'clusterx' command"},
      {{"CLUSTERX", "NOPE"}, usage},
      {{"CLUSTERX", "SETNODEID", "short"}, usage},
      {{"CLUSTERX", "SETNODES", "x"}, usage},
      {{"CLUSTERX", "SETNODES", "x", "v"}, "ERR Invalid version"},
      {{"CLUSTERX", "SETNODES", "x", "1", "nope"}, "ERR Invalid setnodes options"},
      {{"CLUSTERX", "SETNODES", "x", "1", "force", "more"}, "ERR Invalid setnodes options"},
      {{"CLUSTERX", "SETNODES", "a", "-1"}, "ERR Invalid cluster version"},
      {{"CLUSTERX", "SETSLOT", "16384", kNodeA, "1"}, usage},
      {{"CLUSTERX", "SETSLOT", "0", "NOD", kNodeA, "1"}, "ERR Invalid setslot options"},
      {{"CLUSTERX", "SETSLOT", "0", "NODE", "abc", "1"}, "ERR Invalid node id"},
      {{"CLUSTERX", "SETSLOT", "0", "NODE", kNodeA, "x"}, "ERR value is not an integer or out of range"},
      {{"CLUSTERX", "SETSLOT", "0", "NODE", kNodeA, "-1"}, "ERR Invalid version"},
      {{"CLUSTERX", "SETSLOT", "16384", "NODE", kNodeA, "1"}, "ERR Invalid slot id: out of numeric range"},
      {{"CLUSTERX", "MIGRATE", "1"}, "ERR wrong number of arguments"},
      {{"CLUSTERX", "MIGRATE", "1", kNodeB, "async", "5"}, "ERR Async migration does not support timeout"},
      {{"CLUSTERX", "MIGRATE", "1", kNodeB, "maybe"}, "ERR Invalid sync flag"},
      {{"CLUSTERX", "MIGRATE", "1", kNodeB, "sync", "x"}, "ERR timeout is not an integer or out of range"},
      {{"CLUSTERX", "MIGRATE", "1", kNodeB, "sync", "-1"}, "ERR timeout is negative"},
      {{"CLUSTERX", "MIGRATE", "1", kNodeB}, "ERR Can't find the destination node id"},
      {{"CLUSTERX", "MIGRATE", "0-1", kNodeB, "sync", "10"}, "ERR Can't find the destination node id"},
      {{"CLUSTERX", "FLUSHSLOTS"}, "ERR wrong number of arguments"},
  };
  for (const auto& [argv, error] : cases) EXPECT_EQ(h.Run(argv), Err(error)) << argv.back();
}

TEST(ClusterxCmd, SetSlotMovesSlotsWithTheNextVersion) {
  CommandHarness h;
  ClusterHarness c(h);
  c.Push(kThreeNodes, 1);
  EXPECT_EQ(h.Run({"CLUSTERX", "SETSLOT", "0 5-6", "node", kNodeB, "2"}), kOk);
  EXPECT_EQ(c.manager->Current()->OwnerOf(5)->id, kNodeB);
  EXPECT_EQ(h.Run({"CLUSTERX", "SETSLOT", "7", "NODE", kNodeB, "2"}), Err("ERR Invalid cluster version"));
}

TEST(ClusterxCmd, NeverRunsInsideATransactionOrAScript) {  // D8
  CommandHarness h;
  ClusterHarness c(h);
  ASSERT_EQ(h.Run({"MULTI"}), kOk);
  EXPECT_EQ(h.Run({"CLUSTERX", "VERSION"}), Err("ERR Command not allowed inside a transaction"));
  ASSERT_EQ(h.Run({"DISCARD"}), kOk);
  EXPECT_NE(h.Run({"EVAL", "return redis.call('clusterx','version')", "0"})
                .find("This Redis command is not allowed from script"),
            std::string::npos);
}

TEST(ClusterxCmd,
     SetslotWaitsForTheCleanupItTriggered) {  // gocase "... cleared after setslot", Writer's note 4
  MigrationPair p;
  ASSERT_EQ(p.source.Run({"SET", "b", "1"}), kOk);
  ASSERT_TRUE(RunJob(p, {3300, 3300}).ok());
  EXPECT_EQ(p.source.Run({"KEYS", "*"}), Arr({Bulk("b")}));  // kept after the migration
  ASSERT_EQ(p.source.Run({"CLUSTERX", "SETSLOT", "3300", "NODE", kNodeB, "2"}), kOk);
  EXPECT_EQ(p.source.Run({"KEYS", "*"}), "*0\r\n");  // gone right after the reply
}

}  // namespace
}  // namespace tideskv::test
