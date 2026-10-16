#include "cluster/cluster_views.h"

#include "cluster/cluster_errors.h"

namespace tideskv::cluster {
namespace {

std::string Endpoint(const ClusterNode& n) {
  return n.host + ":" + std::to_string(n.port) + "@" + std::to_string(n.port + kClusterPortIncr);
}

std::string Clock(const Topology& t, int64_t now_ms) {
  return std::to_string(now_ms - 1) + " " + std::to_string(now_ms) + " " + std::to_string(t.version);
}

std::string NodeLine(const Topology& t, size_t i, int64_t now_ms) {
  const ClusterNode& n = t.nodes[i];
  const bool me = static_cast<int>(i) == t.myself;
  const bool master = n.role == NodeRole::kMaster;
  std::string out = n.id + " " + Endpoint(n) + " " + (me ? "myself," : "") + (master ? "master" : "slave");
  if (n.failed) out += ",fail";
  out += " " + (master ? std::string("-") : n.master_id) + " " + Clock(t, now_ms);
  out += me || !n.failed ? " connected" : " disconnected";
  const std::string ranges = master ? FormatRanges(t.SlotsOf(static_cast<int>(i))) : std::string();
  if (!ranges.empty()) out += " " + ranges;
  return out + "\n";
}

std::vector<const ClusterNode*> ReplicasOf(const Topology& t, const std::string& master_id,
                                           bool healthy_only) {
  std::vector<const ClusterNode*> out;
  for (const ClusterNode& n : t.nodes) {
    if (n.role == NodeRole::kSlave && n.master_id == master_id && (!healthy_only || !n.failed))
      out.push_back(&n);
  }
  return out;
}

}  // namespace

std::string ClusterInfoText(const Topology& t) {
  const std::string assigned = std::to_string(t.AssignedSlots());
  const std::string version = std::to_string(t.version);
  std::string out;
  const auto line = [&out](std::string_view key, const std::string& value) {
    out.append(key).append(":").append(value).append("\r\n");
  };
  line("cluster_state", "ok");  // Kvrocks: always ok once initialised (the controller ignores it)
  line("cluster_slots_assigned", assigned);
  line("cluster_slots_ok", assigned);
  line("cluster_slots_pfail", "0");
  line("cluster_slots_fail", "0");
  line("cluster_known_nodes", std::to_string(t.nodes.size()));
  line("cluster_size", std::to_string(t.Size()));
  line("cluster_current_epoch", version);
  line("cluster_my_epoch", version);
  return out;  // M7a: no migrating/importing block, never a "-1" range (D12)
}

std::string ClusterNodesText(const Topology& t, int64_t now_ms) {
  std::string out;
  for (size_t i = 0; i < t.nodes.size(); ++i) out += NodeLine(t, i, now_ms);
  return out;
}

Result<std::string> ClusterReplicasText(const Topology& t, std::string_view master_id, int64_t now_ms) {
  const ClusterNode* m = t.Find(master_id);
  if (m == nullptr) return Status::UserError(std::string(kErrInvalidNodeId));
  if (m->role != NodeRole::kMaster) return Status::UserError(std::string(kErrNotAMaster));
  std::string out;
  for (const ClusterNode* n : ReplicasOf(t, m->id, false)) {
    out += n->id + " " + Endpoint(*n) + (n->failed ? " slave,fail " : " slave ") + m->id + " " +
           Clock(t, now_ms) + (n->failed ? " disconnected" : " connected") + "\n";
  }
  return out;
}

std::vector<SlotRun> SlotRuns(const Topology& t) {
  std::vector<SlotRun> out;
  for (uint32_t s = 0; s < kSlotCount; ++s) {
    const int16_t o = t.owner[s];
    if (o < 0) continue;
    if (!out.empty() && out.back().owner == o && out.back().end + 1u == s) {
      out.back().end = static_cast<uint16_t>(s);
      continue;
    }
    const ClusterNode& owner = t.nodes[static_cast<size_t>(o)];
    SlotRun run{static_cast<uint16_t>(s), static_cast<uint16_t>(s), o, {&owner}};
    for (const ClusterNode* r : ReplicasOf(t, owner.id, true)) run.nodes.push_back(r);
    out.push_back(std::move(run));
  }
  return out;
}

std::vector<Shard> Shards(const Topology& t) {
  std::vector<Shard> out;
  for (size_t i = 0; i < t.nodes.size(); ++i) {
    const ClusterNode& n = t.nodes[i];
    if (n.role != NodeRole::kMaster) continue;
    out.push_back({&n, t.SlotsOf(static_cast<int>(i)), ReplicasOf(t, n.id, false)});
  }
  return out;
}

}  // namespace tideskv::cluster
