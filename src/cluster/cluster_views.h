#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cluster/topology.h"
#include "common/status.h"

// The CLUSTER read-only views (M7a D12), pure over a Topology. Texts follow Kvrocks byte for byte where
// kvrocks-controller parses them (INFO: CRLF, one colon per line; NODES: LF, one space between fields,
// field 6 = version, no migration markers).
namespace tideskv::cluster {

std::string ClusterInfoText(const Topology& t);                   // requires t.initialized()
std::string ClusterNodesText(const Topology& t, int64_t now_ms);  // ping_sent now-1, pong now (Kvrocks)
Result<std::string> ClusterReplicasText(const Topology& t, std::string_view master_id, int64_t now_ms);

struct SlotRun {  // CLUSTER SLOTS: a maximal run of slots with one owner
  uint16_t start = 0;
  uint16_t end = 0;
  int16_t owner = -1;
  std::vector<const ClusterNode*> nodes;  // the owner, then its replicas not flagged fail (string order)
};
std::vector<SlotRun> SlotRuns(const Topology& t);

struct Shard {  // CLUSTER SHARDS: a master with its slots and every replica
  const ClusterNode* master = nullptr;
  std::vector<SlotRange> slots;
  std::vector<const ClusterNode*> replicas;
};
std::vector<Shard> Shards(const Topology& t);

}  // namespace tideskv::cluster
