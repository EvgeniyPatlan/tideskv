#include "cluster/slot_router.h"

#include <gtest/gtest.h>

#include <string>

#include "cluster/cluster_errors.h"
#include "cluster/nodes_parser.h"
#include "cluster/slot_states.h"

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

TEST(SlotRouter, AMigratedSlotIsMovedToItsDestinationBeforeAnyPush) {  // D6 step 3
  const Topology t = Three(kA);
  SlotStates s;
  s.migrated.push_back({{3300, 3300}, kB, "10.0.0.9", 7009});
  s.fenced.set(3300);
  EXPECT_EQ(Route(t, s, {"b"}, false, false, false).error, "MOVED 3300 10.0.0.9:7009");
  EXPECT_EQ(Route(t, s, {"b"}, true, false, false).error, "MOVED 3300 10.0.0.9:7009");
  EXPECT_TRUE(Route(t, s, {"bar"}, true, false, false).error.empty());  // 5061 is not migrated
}

TEST(SlotRouter, AFencedSlotRefusesWritesOnly) {  // D4 fast path
  const Topology t = Three(kA);
  SlotStates s;
  s.fenced.set(3300);
  EXPECT_EQ(Route(t, s, {"b"}, true, false, false).error, kErrTryAgainFenced);
  EXPECT_TRUE(Route(t, s, {"b"}, false, false, false).error.empty());
  EXPECT_EQ(Route(t, s, {"b", "foo"}, true, false, false).error, kErrCrossSlot);  // CROSSSLOT first
}

TEST(SlotRouter, AskingReadsAnImportingSlotAndAWriteAnswersTryagain) {  // D6: Kvrocks admits writes
  const Topology t = Three(kB);  // B imports 3300, still A's in the topology
  SlotStates s;
  s.import = ImportJob{{3300, 3300}, ImportState::kStart};
  EXPECT_EQ(Route(t, s, {"b"}, false, false, false).error, "MOVED 3300 127.0.0.1:7000");
  EXPECT_TRUE(Route(t, s, {"b"}, false, false, true).error.empty());
  EXPECT_EQ(Route(t, s, {"b"}, true, false, true).error, kErrTryAgainFenced);
  s.import->state = ImportState::kError;
  EXPECT_EQ(Route(t, s, {"b"}, false, false, true).error, "MOVED 3300 127.0.0.1:7000");
}

TEST(SlotRouter, AnImportedSlotServesEveryoneUntilThePush) {  // D6 step 5
  const Topology t = Three(kB);
  SlotStates s;
  s.imported.push_back({3300, 3300});
  EXPECT_TRUE(Route(t, s, {"b"}, true, false, false).error.empty());
  EXPECT_EQ(Route(t, s, {"bar"}, true, false, false).error, "MOVED 5061 127.0.0.1:7000");
}

TEST(SlotStates, LookupsAndFormatting) {
  SlotStates s;
  s.migrated.push_back({{10, 20}, kB, "h", 1});
  s.import = ImportJob{{30, 30}, ImportState::kStart};
  s.imported.push_back({40, 41});
  SetFenced(s, {10, 20}, true);
  EXPECT_EQ(s.MigratedOf(15)->dst_id, kB);
  EXPECT_EQ(s.MigratedOf(21), nullptr);
  EXPECT_TRUE(s.Fenced(20));
  EXPECT_FALSE(s.Fenced(21));
  EXPECT_TRUE(s.Importing(30));
  EXPECT_TRUE(s.Imported(41));
  EXPECT_FALSE(s.Imported(42));
  SetFenced(s, {10, 15}, false);
  EXPECT_FALSE(s.Fenced(15));
  EXPECT_TRUE(s.Fenced(16));
  EXPECT_EQ(FormatRange({5, 5}), "5");
  EXPECT_EQ(FormatRange({0, 16383}), "0-16383");
  EXPECT_TRUE(Overlaps({1, 5}, {5, 9}));
  EXPECT_FALSE(Overlaps({1, 5}, {6, 9}));
  EXPECT_EQ(StateName(MigrateState::kFail), "fail");
  EXPECT_EQ(StateName(ImportState::kError), "error");
}

}  // namespace
}  // namespace tideskv::cluster
