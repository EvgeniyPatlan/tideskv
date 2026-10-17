#pragma once
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cluster/cluster_manager.h"
#include "commands/cluster_routing.h"
#include "migrate/migrate_settings.h"
#include "migrate/migration_service.h"
#include "migrate/migration_transport.h"
#include "support/command_harness.h"

namespace tideskv::test {

inline const std::string kNodeA(40, 'a');
inline const std::string kNodeB(40, 'b');
inline const std::string kNodeC(40, 'c');
// A owns slots 0-8191 ("b" 3300, "bar" 5061, "c" 7365, "k2" 449, "hello" 866, "w" 3696); B owns 8192-16383
// ("foo" 12182, "a" 15495, "key" 12539, "x" 16287); C replicates A.
inline const std::string kThreeNodes = kNodeA + " 127.0.0.1 7000 master - 0-8191\n" + kNodeB +
                                       " 127.0.0.1 7001 master - 8192-16383\n" + kNodeC +
                                       " 127.0.0.1 7002 slave " + kNodeA + "\n";

// The migrate settings of a harness node, the same mapping App uses (M7b D17; requirepass: KM §5).
inline migrate::MigrateSettings HarnessMigrateSettings(CommandHarness& h) {
  const ConfigValues v = h.services.config_.Snapshot();
  migrate::MigrateSettings s;
  s.sequence_gap = static_cast<uint64_t>(v.migrate_sequence_gap);
  s.batch_bytes = static_cast<size_t>(v.migrate_batch_size_kb) * 1024;
  s.rate_bytes_per_s = static_cast<uint64_t>(v.migrate_batch_rate_limit_mb) << 20;
  s.timeout = std::chrono::seconds(std::max<int64_t>(v.repl_timeout, 1));
  s.requirepass = v.requirepass;
  s.delay = std::chrono::milliseconds(v.migrate_delay_ms);
  return s;
}

// The MigrationService dependencies of a harness node: its engine, log, storage and oids, `cm`, its
// FakeReplication's role, `connect`, its config's migrate settings and its CLIENT PAUSE (M7b D9).
inline migrate::MigrationDeps MigrationDepsOf(CommandHarness& h, cluster::ClusterManager& cm,
                                              migrate::TransportFactory connect) {
  FakeReplication& r = h.services.replication_;
  return migrate::MigrationDeps{
      .engine = h.services.engine_,
      .log = h.services.repl_log_,
      .storage = h.services.storage_,
      .oids = h.services.oids_,
      .cluster = cm,
      .is_replica = [&r] { return r.IsReplica(); },
      .begin_background_write = [&h] { return h.services.pause_.BeginBackgroundWrite(); },
      .end_background_write = [&h] { h.services.pause_.EndWrite(); },
      .make_durable = {},
      .connect = std::move(connect),
      .settings = [&h] { return HarnessMigrateSettings(h); }};
}

// A ClusterManager over the harness's storage whose role hooks drive its FakeReplication, and the node's
// MigrationService; `port` picks which node of kThreeNodes this harness is (7000 = A, 7001 = B, 7002 = C);
// `import_worker_wait` is the service's MigrationDeps::import_worker_wait.
struct ClusterHarness {
  explicit ClusterHarness(CommandHarness& h, uint16_t port = 7000,
                          std::chrono::milliseconds import_worker_wait = std::chrono::seconds(10))
      : h_(h) {
    cluster::ClusterManager::Options o;
    o.port = port;
    o.bind = "127.0.0.1";
    o.local_addresses = [] { return std::vector<std::string>{"127.0.0.1"}; };
    FakeReplication& r = h.services.replication_;
    o.is_replica = [&r] { return r.IsReplica(); };
    o.become_replica = [&r](const std::string& host, uint16_t p) { return r.BecomeReplica(host, p); };
    o.become_master = [this, &r] {
      const Status st = r.BecomeMaster();
      if (st.ok() && migration) migration->OnPromoted();  // as App (M7b D8)
      return st;
    };
    o.on_publish = [this](const std::shared_ptr<const cluster::Topology>&) { RefreshRedirect(); };
    o.on_states_publish = [this](const std::shared_ptr<const cluster::SlotStates>&) { RefreshRedirect(); };
    o.stop_migration = [this] {  // as App (M7b D7)
      if (migration) migration->OnDemoted();
    };
    o.on_transition = [this](const std::shared_ptr<const cluster::Topology>&,
                             const std::shared_ptr<const cluster::Topology>&) {
      if (migration) migration->OnTopology();
    };
    manager = std::make_unique<cluster::ClusterManager>(h.services.storage_, std::move(o));
    EXPECT_TRUE(manager->Load().ok());
    h.services.cluster_manager = manager.get();
    migrate::MigrationDeps deps = MigrationDepsOf(
        h, *manager,
        [this](const std::string& host, uint16_t p, std::chrono::milliseconds t,
               const std::atomic<bool>* cancel) -> Result<std::unique_ptr<migrate::MigrationTransport>> {
          if (!transport) return Status::IOError("no transport");
          return transport(host, p, t, cancel);
        });
    deps.import_worker_wait = import_worker_wait;
    migration = std::make_unique<migrate::MigrationService>(std::move(deps));
    h.services.migration_service = migration.get();
  }
  ~ClusterHarness() {
    h_.services.migration_service = nullptr;
    h_.services.cluster_manager = nullptr;
    h_.services.blocking_.SetRedirect({});
  }
  ClusterHarness(const ClusterHarness&) = delete;
  ClusterHarness& operator=(const ClusterHarness&) = delete;

  void Push(const std::string& nodes, int64_t version) {
    ASSERT_TRUE(manager->SetNodes(nodes, version, false).ok());
  }

  // What App::RefreshRedirect does (D16, M7b D18).
  void RefreshRedirect() {
    const std::shared_ptr<const cluster::Topology> t = manager->Current();
    const std::shared_ptr<const cluster::SlotStates> s = manager->States();
    h_.services.blocking_.SetRedirect([t, s](const BlockedClient& c) { return BlockedRedirect(*t, *s, c); });
  }

  CommandHarness& h_;
  std::unique_ptr<cluster::ClusterManager> manager;
  migrate::TransportFactory transport;                   // set by tests; none: connects fail
  std::unique_ptr<migrate::MigrationService> migration;  // over manager (M7b); null only while constructing
};

}  // namespace tideskv::test
