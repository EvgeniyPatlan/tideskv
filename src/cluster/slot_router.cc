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

std::string Moved(uint16_t slot, const std::string& host, uint16_t port) {
  return std::string(kMovedPrefix)
      .append(std::to_string(slot))
      .append(" ")
      .append(host)
      .append(":")
      .append(std::to_string(port));
}

// D6 steps 3-7, after CROSSSLOT and CLUSTERDOWN.
std::string Decide(const Topology& t, const SlotStates& s, uint16_t slot, const ClusterNode& owner,
                   bool is_write, bool readonly_session, bool asking) {
  const ClusterNode* me = t.Myself();
  if (me == &owner) {
    if (const MigratedRange* m = s.MigratedOf(slot)) return Moved(slot, m->dst_host, m->dst_port);
    if (is_write && s.Fenced(slot)) return std::string(kErrTryAgainFenced);
    return {};
  }
  if (asking && s.Importing(slot)) return is_write ? std::string(kErrTryAgainFenced) : std::string();
  if (s.Imported(slot)) return {};
  if (ServesReplicaRead(me, owner, is_write, readonly_session)) return {};
  return Moved(slot, owner.host, owner.port);
}

}  // namespace

RouteDecision Route(const Topology& t, const SlotStates& s, const std::vector<std::string_view>& keys,
                    bool is_write, bool readonly_session, bool asking) {
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
  d.error = Decide(t, s, slot, *owner, is_write, readonly_session, asking);
  return d;
}

RouteDecision Route(const Topology& t, const std::vector<std::string_view>& keys, bool is_write,
                    bool readonly_session) {
  static const SlotStates kNone;
  return Route(t, kNone, keys, is_write, readonly_session, /*asking=*/false);
}

}  // namespace tideskv::cluster
