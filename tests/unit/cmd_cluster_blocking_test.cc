#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "commands/cluster_routing.h"
#include "support/cluster_harness.h"

namespace tideskv::test {
namespace {

using Replies = std::vector<std::string>;
const std::string kAllOnB =
    kNodeA + " 127.0.0.1 7000 master -\n" + kNodeB + " 127.0.0.1 7001 master - 0-16383\n";

TEST(ClusterBlocking, AWaitOnASlotThatMovesAwayEndsWithMovedAndKeepsTheConnection) {  // cluster.c:7607-7666
  CommandHarness h;
  ClusterHarness c(h);
  c.Push(kThreeNodes, 1);
  EXPECT_EQ(h.Run({"BLPOP", "b", "0"}), "");
  ASSERT_NE(h.session.blocked, nullptr);
  c.Push(kAllOnB, 2);
  EXPECT_EQ(h.TakeUnblocked(), (Replies{Err("MOVED 3300 127.0.0.1:7001")}));
  EXPECT_EQ(h.link->closes(), 0);
  EXPECT_EQ(h.services.blocking_.blocked_clients(), 0u);
}

TEST(ClusterBlocking, AnUnservedSlotEndsWithClusterdownAndWaitsOnOwnedSlotsStay) {
  CommandHarness h;
  ClusterHarness c(h);
  c.Push(kThreeNodes, 1);
  SecondClient other(h);
  EXPECT_EQ(h.Run({"BLPOP", "b", "0"}), "");      // slot 3300
  EXPECT_EQ(other.Run({"BLPOP", "c", "0"}), "");  // slot 7365
  c.Push(kNodeA + " 127.0.0.1 7000 master - 7000-8191\n", 2);
  EXPECT_EQ(h.TakeUnblocked(), (Replies{Err("CLUSTERDOWN Hash slot not served")}));
  EXPECT_TRUE(other.TakeUnblocked().empty());
  EXPECT_EQ(h.services.blocking_.blocked_clients(), 1u);
}

// D16: a command routed under the old topology that registers its wait after the publish is checked at
// insertion (the race Redis closes with its clients cron).
TEST(ClusterBlocking, AWaitRegisteredAfterThePublishIsCheckedAtInsertion) {
  CommandHarness h;
  ClusterHarness c(h);
  c.Push(kThreeNodes, 1);
  c.Push(kAllOnB, 2);
  const ArgList argv{"BLPOP", "b", "0"};
  ConnectionBlockHandler block(h.services.blocking_, h.session, argv, h.ready);
  BlockRequest req;
  req.keys = {"b"};
  block.Block(req);  // what BLPOP's handler does when the list is empty
  EXPECT_EQ(h.TakeUnblocked(), (Replies{Err("MOVED 3300 127.0.0.1:7001")}));
  EXPECT_EQ(h.services.blocking_.blocked_clients(), 0u);
  EXPECT_EQ(h.link->closes(), 0);
}

TEST(ClusterBlocking, AWaitWithoutKeysIsNeverRedirected) {  // WAIT
  BlockingRegistry registry;
  const BlockedClient waiter(registry, Session{}, {"WAIT", "1", "0"}, encoding::ValueType::kList, {},
                             std::nullopt);
  EXPECT_FALSE(BlockedRedirect(cluster::Topology{}, waiter).has_value());
}

}  // namespace
}  // namespace tideskv::test
