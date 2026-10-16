#include "cluster/topology.h"

#include <gtest/gtest.h>

#include <string>

namespace tideskv::cluster {
namespace {

Topology Sample() {
  Topology t;
  t.version = 3;
  t.nodes = {{std::string(40, 'a'), "h", 1, NodeRole::kMaster, false, ""},
             {std::string(40, 'b'), "h", 2, NodeRole::kMaster, false, ""},
             {std::string(40, 'c'), "h", 3, NodeRole::kSlave, false, std::string(40, 'a')},
             {std::string(40, 'd'), "h", 4, NodeRole::kMaster, false, ""}};  // an empty shard
  for (uint16_t s : {0, 1, 2, 4, 10000}) t.owner[s] = 0;
  for (uint16_t s = 5; s < 8; ++s) t.owner[s] = 1;
  return t;
}

TEST(ClusterTopology, SlotsOfMergesRunsInSlotOrder) {
  const Topology t = Sample();
  EXPECT_EQ(t.SlotsOf(0), (std::vector<SlotRange>{{0, 2}, {4, 4}, {10000, 10000}}));
  EXPECT_EQ(FormatRanges(t.SlotsOf(0)), "0-2 4 10000");
  EXPECT_EQ(FormatRanges(t.SlotsOf(3)), "");
}

TEST(ClusterTopology, CountsAndLookups) {
  Topology t = Sample();
  EXPECT_EQ(t.AssignedSlots(), 8u);
  EXPECT_EQ(t.Size(), 2u);  // masters owning at least one slot
  EXPECT_EQ(t.OwnerOf(5)->id, std::string(40, 'b'));
  EXPECT_EQ(t.OwnerOf(3), nullptr);
  EXPECT_EQ(t.IndexOf(std::string(40, 'c')), 2);
  EXPECT_EQ(t.Find("nope"), nullptr);
  t.myid = std::string(40, 'c');
  t.ResolveMyself();
  EXPECT_EQ(t.Myself()->port, 3);
  t.version = kNoVersion;  // myid without a topology: absent
  t.ResolveMyself();
  EXPECT_EQ(t.Myself(), nullptr);
}

}  // namespace
}  // namespace tideskv::cluster
