#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cluster/topology.h"
#include "common/status.h"
#include "storage/storage.h"

// The node's cluster state (M7a D3, D5-D7, D10, D20): the topology the controller pushed, persisted in
// sys/cluster_cfg, and the replication role it implies. Changes are serialised by apply_mu_ and run on the
// CLUSTERX thread (never under the Engine gate, D8): validate -> role hooks -> persist + make_durable ->
// publish. Any failure returns before the new version is published, so the controller re-pushes.
// Lock order: apply_mu_ -> (hooks: ReplicationManager::role_mu_ ...) ; topo_mu_ is a leaf.
namespace tideskv::cluster {

struct Endpoint {
  std::string host;
  uint16_t port = 0;
};

class ClusterManager {
 public:
  struct Options {
    uint16_t port = 0;  // this node's listening port (myid detection, D6)
    std::string bind;   // the bind config (myid detection)
    std::function<std::vector<std::string>()> local_addresses;  // null: LocalInterfaceAddresses
    // Role hooks (D10), wired by App to ReplicationManager. Every hook is optional.
    std::function<bool()> is_replica;
    std::function<bool(const std::string& host, uint16_t port)> become_replica;  // false: same master
    std::function<Status()> become_master;
    std::function<Status()> make_durable;  // after each sys txn (D20: storage Checkpoint)
    // After every publish, with the new topology (D16: blocked clients follow it).
    std::function<void(const std::shared_ptr<const Topology>&)> on_publish;
  };

  ClusterManager(Storage& storage, Options options);
  ClusterManager(const ClusterManager&) = delete;
  ClusterManager& operator=(const ClusterManager&) = delete;

  // Startup, before replication exists: reads sys/cluster_cfg and publishes it. Changes no role.
  Status Load();
  // The published topology; one atomic load while it is unchanged (thread-local cache, D3).
  std::shared_ptr<const Topology> Current() const;
  // The master to replicate from at boot: myself is a slave of a node in the topology; nullopt = a master.
  std::optional<Endpoint> BootMaster() const;

  Status SetNodes(std::string_view nodes, int64_t version, bool force);  // CLUSTERX SETNODES (D5, D7)
  Status SetNodeId(std::string_view id);                                 // CLUSTERX SETNODEID (D6)
  Status SetSlot(const std::vector<SlotRange>& ranges, std::string_view node_id, int64_t version);
  Status Reset(bool keyspace_empty);  // CLUSTER RESET (D11)

 private:
  Status ApplyRole(const Topology& next) const;  // D10, under apply_mu_
  Status Promote() const;                        // become_master, its failure in Kvrocks's words (D7)
  Status Commit(const std::shared_ptr<const Topology>& next);  // persist, make durable, publish
  void Publish(const std::shared_ptr<const Topology>& next);
  std::optional<std::string> DetectMyId(const Topology& t) const;

  Storage& storage_;
  Options options_;
  const uint64_t instance_;  // process-unique: keys the thread-local cache of Current()
  std::mutex apply_mu_;
  mutable std::mutex topo_mu_;  // guards topo_; generation_ is written under it
  std::shared_ptr<const Topology> topo_;
  std::atomic<uint64_t> generation_{0};
};

}  // namespace tideskv::cluster
