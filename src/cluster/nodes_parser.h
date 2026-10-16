#pragma once
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "cluster/topology.h"
#include "common/status.h"

namespace tideskv::cluster {

struct ParsedNodes {
  std::vector<ClusterNode> nodes;         // in string order
  std::array<int16_t, kSlotCount> owner;  // -1 = not served
};

// Kvrocks Cluster::parseClusterNodes (v2.17.0 cluster.cc:736-864; M7a D4). UserError with Kvrocks's text.
Result<ParsedNodes> ParseNodes(std::string_view text);
// The SETNODES string of `t` (every line "\n"-terminated; "fail" kept): ParseNodes(SerializeNodes(t)) gives
// t's nodes and owner back.
std::string SerializeNodes(const Topology& t);
// Kvrocks CommandTable::ParseSlotRanges (commander.cc:119-165): SETSLOT / MIGRATE / FLUSHSLOTS slot lists,
// space-separated "N" or "a-b" (a <= b).
Result<std::vector<SlotRange>> ParseSlotRanges(std::string_view text);

}  // namespace tideskv::cluster
