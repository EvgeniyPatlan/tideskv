#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cluster/slot_states.h"
#include "commands/blocking_serve.h"
#include "commands/cluster_routing.h"
#include "encoding/meta_codec.h"
#include "encoding/object_codec.h"
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

TEST(ClusterBlocking, AFenceKeepsAWaitAndTheSuccessPublishMovesIt) {  // D18
  CommandHarness h;
  ClusterHarness c(h);
  c.Push(kThreeNodes, 1);
  SecondClient other(h);
  EXPECT_EQ(h.Run({"BLPOP", "b", "0"}), "");        // slot 3300
  EXPECT_EQ(other.Run({"BLPOP", "bar", "0"}), "");  // slot 5061
  auto fenced = std::make_shared<cluster::SlotStates>();
  fenced->fenced.set(3300);
  c.manager->PublishStates(fenced);
  EXPECT_TRUE(h.TakeUnblocked().empty()) << "the fence is short: the wait stays";
  auto success = std::make_shared<cluster::SlotStates>(*fenced);
  success->migrated.push_back({{3300, 3300}, kNodeB, "127.0.0.1", 7001});
  c.manager->PublishStates(success);
  EXPECT_EQ(h.TakeUnblocked(), (Replies{Err("MOVED 3300 127.0.0.1:7001")}));
  EXPECT_TRUE(other.TakeUnblocked().empty());
  EXPECT_EQ(h.link->closes(), 0);
}

// D18, the M7a ledger carry: the ASKING flag of a blocking command belongs to the wait, and routes it.
TEST(ClusterBlocking, AskingIsCarriedIntoABlockedWait) {
  CommandHarness h;
  ClusterHarness c(h);
  c.Push(kThreeNodes, 1);  // A owns 3300
  SecondClient plain(h);
  ASSERT_EQ(h.Run({"ASKING"}), kOk);
  EXPECT_EQ(h.Run({"BLPOP", "b", "0"}), "");
  EXPECT_EQ(plain.Run({"BLPOP", "{b}x", "0"}), "");
  ASSERT_NE(h.session.blocked, nullptr);
  EXPECT_TRUE(h.session.blocked->session().asking);
  EXPECT_FALSE(h.session.asking);  // the connection's flag covered exactly that command
  auto importing = std::make_shared<cluster::SlotStates>();
  importing->import = cluster::ImportJob{{3300, 3300}, cluster::ImportState::kStart};
  c.manager->PublishStates(importing);
  c.Push(kAllOnB, 2);  // B owns 3300 now, and A imports it back
  EXPECT_EQ(plain.TakeUnblocked(), (Replies{Err("MOVED 3300 127.0.0.1:7001")}));
  EXPECT_TRUE(h.TakeUnblocked().empty()) << "an ASKING wait on an importing slot keeps waiting";
  auto imported = std::make_shared<cluster::SlotStates>(*importing);
  imported->import->state = cluster::ImportState::kSuccess;
  imported->imported.push_back({3300, 3300});
  c.manager->PublishStates(imported);
  EXPECT_TRUE(h.TakeUnblocked().empty()) << "imported: served here from now on";
}

TEST(ClusterBlocking, AServeRefusedByTheFenceStaysBlocked) {  // D18
  CommandHarness h;
  ClusterHarness c(h);
  c.Push(kThreeNodes, 1);
  EXPECT_EQ(h.Run({"BLPOP", "b", "0"}), "");
  h.services.engine_.FenceSlots(3300, 3300);  // the fence lands between a push's commit and its serve
  encoding::Metadata list;
  list.type = encoding::ValueType::kList;
  ASSERT_TRUE(MoveInto(h.services.oids_.Next(), &list.oid).ok());
  list.size = 1;
  list.head = list.tail = encoding::kListInitialIndex;
  ASSERT_TRUE(
      h.services.engine_
          .ApplyImport({{encoding::ReplOpType::kPut, ColumnFamily::kMeta, encoding::MetaKeyFor(0, "b"),
                         encoding::EncodeMetadata(list)},
                        {encoding::ReplOpType::kPut, ColumnFamily::kSubkey,
                         encoding::EncodeSubkey(0, 3300, list.oid,
                                                encoding::EncodeListIndex(encoding::kListInitialIndex)),
                         "v"}})
          .ok());  // the push, committed (ApplyImport is unfenced)
  ReadyKeys ready{{0, "b"}};
  ServeBlockedClients(h.services.registry(), h.services, ready);
  EXPECT_TRUE(h.TakeUnblocked().empty()) << "refused by the fence: the wait stays";
  EXPECT_EQ(h.services.blocking_.blocked_clients(), 1u);
  h.services.engine_.UnfenceSlots(3300, 3300);
  ReadyKeys again{{0, "b"}};
  ServeBlockedClients(h.services.registry(), h.services, again);
  EXPECT_EQ(h.TakeUnblocked(), (Replies{Arr({Bulk("b"), Bulk("v")})}));
}

}  // namespace
}  // namespace tideskv::test
