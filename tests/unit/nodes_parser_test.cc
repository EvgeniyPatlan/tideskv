#include "cluster/nodes_parser.h"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "cluster/cluster_errors.h"

namespace tideskv::cluster {
namespace {

const std::string kA(40, 'a');
const std::string kB(40, 'b');
const std::string kC(40, 'c');
const std::string kNodes = kA + " 10.0.0.1 7000 master - 0-8191\n" + kB +
                           " 10.0.0.2 7001 master - 8192-16383\n" + kC + " 10.0.0.3 7002 slave " + kA + "\n";

TEST(NodesParser, ParsesTheControllersFormat) {  // kvrocks-cluster §1.3
  auto p = ParseNodes(kNodes);
  ASSERT_TRUE(p.ok()) << p.status().ToString();
  ASSERT_EQ(p->nodes.size(), 3u);
  EXPECT_EQ(p->nodes[0], (ClusterNode{kA, "10.0.0.1", 7000, NodeRole::kMaster, false, ""}));
  EXPECT_EQ(p->nodes[2], (ClusterNode{kC, "10.0.0.3", 7002, NodeRole::kSlave, false, kA}));
  EXPECT_EQ(p->owner[0], 0);
  EXPECT_EQ(p->owner[8191], 0);
  EXPECT_EQ(p->owner[8192], 1);
  EXPECT_EQ(p->owner[16383], 1);
}

TEST(NodesParser, ToleratesWhatKvrocksSplitDrops) {                   // util::Split drops empty tokens
  const std::string id = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx01";  // any 40 bytes (controller ids)
  auto p =
      ParseNodes("\n\n" + id + "  127.0.0.1   7000 master -  0-1   3 5-8191 \n\n" + kB + " h 1 master -\n");
  ASSERT_TRUE(p.ok()) << p.status().ToString();
  EXPECT_EQ(p->nodes[0].id, id);
  EXPECT_EQ(p->owner[2], -1);
  EXPECT_EQ(p->owner[3], 0);
  EXPECT_EQ(p->nodes.size(), 2u);                   // a master with no slots is valid (an empty shard)
  auto dash = ParseNodes(kA + " h 1 master - 5-");  // Kvrocks quirk: "5-" splits to {"5"}
  ASSERT_TRUE(dash.ok());
  EXPECT_EQ(dash->owner[5], 0);
}

TEST(NodesParser, FlagsAreCommaSeparatedAndTheLastRoleWins) {
  auto p = ParseNodes(kA + " h 1 myself,MASTER - 0\n" + kB + " h 2 slave,fail " + kA + "\n" + kC +
                      " h 3 master,replica " + kA);
  ASSERT_TRUE(p.ok()) << p.status().ToString();
  EXPECT_EQ(p->nodes[0].role, NodeRole::kMaster);
  EXPECT_TRUE(p->nodes[1].failed);
  EXPECT_EQ(p->nodes[2].role, NodeRole::kSlave);
}

TEST(NodesParser, ErrorTextsAreKvrocks) {  // cluster.cc:736-864
  const std::string hex1 = "07c37dfeb235213a872192d90877d0cd55635b91";
  const std::string hex2 = "07c37dfeb235213a872192d90877d0cd55635b92";
  const std::vector<std::pair<std::string, std::string_view>> cases = {
      {"", kErrInvalidNodesInfo},
      {" \n  ", kErrInvalidNodesInfo},
      {"short h 1 master -", kErrInvalidNodeId},
      {kA + " h 70000 master -", kErrInvalidNodePort},
      {kA + " h x master -", kErrInvalidNodePort},
      {kA + " h 1 myself -", kErrInvalidNodeRole},
      {kA + " h 1 master x", kErrInvalidNodeId},
      {kA + " h 1 slave abc", kErrInvalidNodeId},
      {kA + " h 1 slave " + kB + " 5", kErrInvalidNodesInfo},
      {kA + " h 1 master - 16384", kErrSlotOutOfRange},
      {kA + " h 1 master - 5-5", kErrSlotOutOfRange},
      {kA + " h 1 master - 9-3", kErrSlotOutOfRange},
      {kA + " h 1 master - 1-2-3", kErrSlotOutOfRange},
      {kA + " h 1 master - x", kErrSlotOutOfRange},
      {kA + " h 1 master - -", kErrSlotOutOfRange},
      {kA + " h 1 master - 0-10\n" + kB + " h 2 master - 10", kErrSlotOverlapped},
      {hex1 + " h 1 master - 0-100 " + hex2 + " h 2 slave " + hex1, kErrMissingNewline},
      {kA + " h 1 master -\n" + kA + " h 2 master -", kErrInvalidNodesInfo},  // D4: duplicate id
  };
  for (const auto& [text, error] : cases) {
    auto p = ParseNodes(text);
    ASSERT_FALSE(p.ok()) << text;
    EXPECT_EQ(p.status().code(), StatusCode::kUserError);
    EXPECT_EQ(p.status().message(), error) << text;
  }
}

TEST(NodesParser, SerializeWritesTheSetnodesFormat) {
  auto p = ParseNodes(kA + " h 1 master - 3 0-1 5-6\n" + kB + " h 2 slave,fail " + kA);
  ASSERT_TRUE(p.ok());
  Topology t;
  t.version = 1;
  t.nodes = p->nodes;
  t.owner = p->owner;
  EXPECT_EQ(SerializeNodes(t), kA + " h 1 master - 0-1 3 5-6\n" + kB + " h 2 slave,fail " + kA + "\n");
}

RC_GTEST_PROP(NodesParserProp, SerializeThenParseIsTheIdentity, (std::vector<uint8_t> owners)) {
  Topology t;
  t.version = 7;
  t.nodes = {{kA, "h1", 1, NodeRole::kMaster, false, ""},
             {kB, "h2", 2, NodeRole::kMaster, true, ""},
             {kC, "h3", 3, NodeRole::kSlave, false, kA}};
  for (size_t s = 0; s < owners.size() && s < kSlotCount; ++s) {
    t.owner[s] = owners[s] % 3 == 2 ? int16_t{-1} : static_cast<int16_t>(owners[s] % 3);
  }
  auto p = ParseNodes(SerializeNodes(t));
  RC_ASSERT(p.ok());
  RC_ASSERT(p->nodes == t.nodes);
  RC_ASSERT(p->owner == t.owner);
}

TEST(SlotRanges, ParseKvrocksSlotLists) {  // commander.cc:119-165
  auto r = ParseSlotRanges("1-3 4  16383");
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_EQ(*r, (std::vector<SlotRange>{{1, 3}, {4, 4}, {16383, 16383}}));
  ASSERT_TRUE(ParseSlotRanges("7-7").ok());  // a-a is fine here (unlike the nodes string)
}

TEST(SlotRanges, ErrorTextsAreKvrocks) {
  EXPECT_EQ(ParseSlotRanges("").status().message(), "ERR No slots to parse.");
  EXPECT_EQ(ParseSlotRanges("   ").status().message(),
            "ERR Invalid slots: `   `. No slots to parse. Please use spaces to separate slots.");
  EXPECT_EQ(ParseSlotRanges("16384").status().message(), "ERR Invalid slot id: out of numeric range");
  EXPECT_EQ(ParseSlotRanges("x").status().message(), "ERR Invalid slot id: encounter non-integer characters");
  EXPECT_EQ(ParseSlotRanges("-3").status().message(),
            "ERR Invalid slot range: `-3`. The character '-' can't appear in the first or last position.");
  EXPECT_EQ(ParseSlotRanges("1-2-3").status().message(),
            "ERR Invalid slot range: `1-2-3`. The slot range should be of the form `int1-int2`.");
  EXPECT_EQ(ParseSlotRanges("5-3").status().message(),
            "ERR Invalid slot range: `5-3`. The slot range `int1-int2` needs to satisfy the condition (int1 "
            "<= int2).");
}

}  // namespace
}  // namespace tideskv::cluster
