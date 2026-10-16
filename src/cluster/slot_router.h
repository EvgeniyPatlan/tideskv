#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "cluster/topology.h"

// Kvrocks Cluster::CanExecByMySelf without the migration branches (M7a D13), with Redis 7.2's reply texts.
namespace tideskv::cluster {

struct RouteDecision {
  int slot = -1;      // the keys' slot; -1 when there are none
  std::string error;  // empty: run here; else the reply text (no leading '-')
};

// keys empty -> run. Several slots -> CROSSSLOT. Uninitialised or unserved -> CLUSTERDOWN Hash slot not
// served. Owner == myself -> run. A READONLY session on a slave of the owner runs non-writes. Else MOVED to
// the owner.
RouteDecision Route(const Topology& t, const std::vector<std::string_view>& keys, bool is_write,
                    bool readonly_session);

}  // namespace tideskv::cluster
