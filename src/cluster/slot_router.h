#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "cluster/slot_states.h"
#include "cluster/topology.h"

// Routing of one call (M7a D13, M7b D6).
namespace tideskv::cluster {

struct RouteDecision {
  int slot = -1;      // the keys' slot; -1 when there are none
  std::string error;  // empty: run here; else the reply text (no leading '-')
};

// Kvrocks Cluster::CanExecByMySelf with the M7b migration branches (D6), with Redis 7.2's reply texts:
// keys empty -> run; several slots -> CROSSSLOT; uninitialised or unserved -> CLUSTERDOWN Hash slot not
// served; owner == myself -> MOVED to the destination if migrated, TRYAGAIN for a write if fenced, else run;
// importing + ASKING -> run a read, TRYAGAIN a write; imported -> run; a READONLY session on a slave of the
// owner runs non-writes; else MOVED to the owner.
RouteDecision Route(const Topology& t, const SlotStates& s, const std::vector<std::string_view>& keys,
                    bool is_write, bool readonly_session, bool asking);
// The M7a form: no migration states, no ASKING.
RouteDecision Route(const Topology& t, const std::vector<std::string_view>& keys, bool is_write,
                    bool readonly_session);

}  // namespace tideskv::cluster
