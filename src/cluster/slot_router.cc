#include "cluster/slot_router.h"

#include <cstdint>

#include "cluster/cluster_errors.h"
#include "common/crc16.h"

namespace tideskv::cluster {
namespace {

// Redis getNodeByQuery's READONLY branch (cluster.c): a replica of the slot's owner serves non-writes.
bool ServesReplicaRead(const ClusterNode* me, const ClusterNode& owner, bool is_write,
                       bool readonly_session) {
  return me != nullptr && !is_write && readonly_session && me->role == NodeRole::kSlave &&
         me->master_id == owner.id;
}

}  // namespace

RouteDecision Route(const Topology& t, const std::vector<std::string_view>& keys, bool is_write,
                    bool readonly_session) {
  RouteDecision d;
  if (keys.empty()) return d;
  const uint16_t slot = KeySlot(keys.front());
  for (size_t i = 1; i < keys.size(); ++i) {
    if (KeySlot(keys[i]) != slot) {
      d.error = std::string(kErrCrossSlot);
      return d;
    }
  }
  d.slot = slot;
  const ClusterNode* owner = t.initialized() ? t.OwnerOf(slot) : nullptr;
  if (owner == nullptr) {
    d.error = std::string(kErrSlotNotServed);
    return d;
  }
  const ClusterNode* me = t.Myself();
  if (me == owner || ServesReplicaRead(me, *owner, is_write, readonly_session)) return d;
  d.error = std::string(kMovedPrefix)
                .append(std::to_string(slot))
                .append(" ")
                .append(owner->host)
                .append(":")
                .append(std::to_string(owner->port));
  return d;
}

}  // namespace tideskv::cluster
