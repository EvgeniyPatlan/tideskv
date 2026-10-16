#include "cluster/cluster_manager.h"

#include <spdlog/spdlog.h>

#include <utility>

#include "cluster/cluster_errors.h"
#include "cluster/cluster_store.h"
#include "cluster/local_addrs.h"
#include "cluster/nodes_parser.h"

namespace tideskv::cluster {
namespace {

std::atomic<uint64_t> g_next_instance{1};

Status Err(std::string_view text) { return Status::UserError(std::string(text)); }

}  // namespace

ClusterManager::ClusterManager(Storage& storage, Options options)
    : storage_(storage),
      options_(std::move(options)),
      instance_(g_next_instance.fetch_add(1)),
      topo_(std::make_shared<const Topology>()) {}

std::shared_ptr<const Topology> ClusterManager::Current() const {
  struct Cache {
    uint64_t instance = 0;
    uint64_t generation = 0;
    std::shared_ptr<const Topology> topo;
  };
  thread_local Cache cache;
  if (cache.instance != instance_ || cache.generation != generation_.load(std::memory_order_acquire)) {
    std::lock_guard lock(topo_mu_);
    cache = {instance_, generation_.load(std::memory_order_relaxed), topo_};
  }
  return cache.topo;
}

void ClusterManager::Publish(const std::shared_ptr<const Topology>& next) {
  {
    std::lock_guard lock(topo_mu_);
    topo_ = next;
    generation_.fetch_add(1, std::memory_order_release);
  }
  if (options_.on_publish) options_.on_publish(next);
}

Status ClusterManager::Load() {
  auto loaded = LoadClusterCfg(storage_);
  if (!loaded.ok()) return loaded.status();
  const std::shared_ptr<Topology> t = std::move(loaded).value();
  if (t->initialized())
    spdlog::info("cluster topology version {} loaded; this node is {}", t->version, t->myid);
  Publish(t);
  return Status::Ok();
}

std::optional<Endpoint> ClusterManager::BootMaster() const {
  const auto t = Current();
  const ClusterNode* me = t->Myself();
  if (me == nullptr || me->role != NodeRole::kSlave) return std::nullopt;
  const ClusterNode* m = t->Find(me->master_id);
  if (m == nullptr) return std::nullopt;
  return Endpoint{m->host, m->port};
}

std::optional<std::string> ClusterManager::DetectMyId(const Topology& t) const {
  const std::vector<std::string> local =
      options_.local_addresses ? options_.local_addresses() : LocalInterfaceAddresses();
  for (const ClusterNode& n : t.nodes) {
    if (n.port == options_.port && MatchListeningIp(options_.bind, n.host, local)) return n.id;
  }
  return std::nullopt;
}

Status ClusterManager::Promote() const {
  if (!options_.is_replica || !options_.is_replica() || !options_.become_master) return Status::Ok();
  const Status st = options_.become_master();
  if (st.ok()) return Status::Ok();
  // D7/D22: a user error (e.g. LOADING) is the promotion's own reply text; anything else exposes no
  // internals.
  const std::string reason =
      st.code() == StatusCode::kUserError ? st.message() : std::string(kReplicationReasonStorage);
  spdlog::warn("cluster topology asks for a master, but the promotion failed: {}", st.ToString());
  return Status::UserError(std::string(kErrClusterReplication) + reason);
}

// D10: a slave of a known master follows it (a no-op for the same master); a master, or a node absent from
// the topology, stops replicating; a slave of an unknown master keeps its role (Kvrocks).
Status ClusterManager::ApplyRole(const Topology& next) const {
  const ClusterNode* me = next.Myself();
  if (me == nullptr || me->role == NodeRole::kMaster) return Promote();
  const ClusterNode* master = next.Find(me->master_id);
  if (master != nullptr && options_.become_replica) options_.become_replica(master->host, master->port);
  return Status::Ok();
}

Status ClusterManager::Commit(const std::shared_ptr<const Topology>& next) {
  if (Status st = SaveClusterCfg(storage_, *next); !st.ok()) return st;
  if (options_.make_durable) {
    if (Status st = options_.make_durable(); !st.ok()) return st;
  }
  Publish(next);
  return Status::Ok();
}

Status ClusterManager::SetNodes(std::string_view nodes, int64_t version, bool force) {
  std::lock_guard apply(apply_mu_);
  const std::shared_ptr<const Topology> cur = Current();
  if (version < 0) return Err(kErrInvalidClusterVersion);
  if (!force && cur->version > version) return Err(kErrInvalidClusterVersion);
  if (!force && cur->version == version) return Status::Ok();  // D5: no parse, no change
  auto parsed = ParseNodes(nodes);
  if (!parsed.ok()) return parsed.status();
  auto next = std::make_shared<Topology>();
  next->version = version;
  next->nodes = std::move(parsed->nodes);
  next->owner = parsed->owner;
  next->myid = cur->myid;
  if (cur->myid.empty() || force) {
    if (auto detected = DetectMyId(*next)) next->myid = *detected;
  }
  next->ResolveMyself();
  if (Status st = ApplyRole(*next); !st.ok()) return st;
  return Commit(next);
}

Status ClusterManager::SetNodeId(std::string_view id) {
  if (id.size() != kNodeIdLength) return Err(kErrInvalidNodeId);
  std::lock_guard apply(apply_mu_);
  const std::shared_ptr<const Topology> cur = Current();
  if (cur->myid == id) return ApplyRole(*cur);  // the controller resends it each sync: no checkpoint
  auto next = std::make_shared<Topology>(*cur);
  next->myid = std::string(id);
  next->ResolveMyself();
  if (Status st = ApplyRole(*next); !st.ok()) return st;
  return Commit(next);
}

Status ClusterManager::SetSlot(const std::vector<SlotRange>& ranges, std::string_view node_id,
                               int64_t version) {
  std::lock_guard apply(apply_mu_);
  const std::shared_ptr<const Topology> cur = Current();
  if (version <= 0 || version != cur->version + 1) return Err(kErrInvalidClusterVersion);
  if (node_id.size() != kNodeIdLength) return Err(kErrInvalidNodeId);
  const int target = cur->IndexOf(node_id);
  if (target < 0) return Err(kErrNoSuchNode);
  if (cur->nodes[static_cast<size_t>(target)].role != NodeRole::kMaster) return Err(kErrNotAMaster);
  auto next = std::make_shared<Topology>(*cur);
  next->version = version;
  for (const SlotRange& r : ranges) {
    // ParseSlotRanges already bounds them; this keeps a direct caller from writing past owner.
    if (r.start > r.end || r.end >= kSlotCount) return Err(kErrSlotOutOfRange);
    for (uint32_t s = r.start; s <= r.end; ++s) next->owner[s] = static_cast<int16_t>(target);
  }
  return Commit(next);  // Kvrocks: no role change on SETSLOT
}

Status ClusterManager::Reset(bool keyspace_empty) {
  std::lock_guard apply(apply_mu_);
  if (!keyspace_empty) return Err(kErrResetNotEmpty);
  if (Status st = Promote(); !st.ok()) return st;
  if (Status st = DeleteClusterCfg(storage_); !st.ok()) return st;
  if (options_.make_durable) {
    if (Status st = options_.make_durable(); !st.ok()) return st;
  }
  Publish(std::make_shared<const Topology>());
  return Status::Ok();
}

}  // namespace tideskv::cluster
