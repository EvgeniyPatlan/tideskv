#include "cluster/slot_router.h"

#include <gtest/gtest.h>

#include <string>

#include "cluster/nodes_parser.h"

namespace tideskv::cluster {
namespace {

const std::string kA(40, 'a');
const std::string kB(40, 'b');
const std::string kC(40, 'c');

Topology Three(const std::string& myid) {
  auto p = ParseNodes(kA + " 127.0.0.1 7000 master - 0-8191\n" + kB +
                      " 127.0.0.1 7001 master - 8192-16383\n" + kC + " 127.0.0.1 7002 slave " + kA + "\n");
  Topology t;
  t.version = 1;
  t.nodes = p->nodes;
  t.owner = p->owner;
  t.myid = myid;
  t.ResolveMyself();
  return t;
}

TEST(SlotRouter, NoKeysRunAnywhere) {
  const RouteDecision d = Route(Topology{}, {}, true, false);
  EXPECT_TRUE(d.error.empty());
  EXPECT_EQ(d.slot, -1);
}

TEST(SlotRouter, OwnedSlotsRunAndForeignOnesAreMoved) {
  const Topology t = Three(kA);
  EXPECT_TRUE(Route(t, {"b"}, true, false).error.empty());
  EXPECT_EQ(Route(t, {"b"}, true, false).slot, 3300);
  EXPECT_EQ(Route(t, {"foo"}, false, false).error, "MOVED 12182 127.0.0.1:7001");
}

TEST(SlotRouter, CrossSlotIsCheckedBeforeOwnership) {
  const Topology t = Three(kA);
  EXPECT_EQ(Route(t, {"b", "foo"}, true, false).error,
            "CROSSSLOT Keys in request don't hash to the same slot");
  EXPECT_TRUE(Route(t, {"{b}1", "{b}2", "b"}, true, false).error.empty());
}

TEST(SlotRouter, UninitialisedAndUnservedSlotsAreClusterdown) {
  EXPECT_EQ(Route(Topology{}, {"b"}, false, false).error, "CLUSTERDOWN Hash slot not served");
  Topology t = Three(kA);
  t.owner[12182] = -1;
  EXPECT_EQ(Route(t, {"foo"}, false, false).error, "CLUSTERDOWN Hash slot not served");
}

TEST(SlotRouter, AReplicaServesOnlyReadonlyReadsOfItsMastersSlots) {
  const Topology t = Three(kC);
  EXPECT_EQ(Route(t, {"b"}, false, false).error, "MOVED 3300 127.0.0.1:7000");
  EXPECT_TRUE(Route(t, {"b"}, false, true).error.empty());
  EXPECT_EQ(Route(t, {"b"}, true, true).error, "MOVED 3300 127.0.0.1:7000");
  EXPECT_EQ(Route(t, {"foo"}, false, true).error, "MOVED 12182 127.0.0.1:7001");
}

TEST(SlotRouter, ARouterNodeMovesEverything) {
  EXPECT_EQ(Route(Three(std::string(40, 'z')), {"b"}, false, true).error, "MOVED 3300 127.0.0.1:7000");
}

}  // namespace
}  // namespace tideskv::cluster
