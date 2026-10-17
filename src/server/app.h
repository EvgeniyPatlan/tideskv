#pragma once
#include <asio.hpp>
#include <chrono>
#include <memory>
#include <optional>

#include "cluster/cluster_manager.h"
#include "commands/registry.h"
#include "commands/server_services.h"
#include "common/clock.h"
#include "exec/engine.h"
#include "exec/lock_manager.h"
#include "exec/repl_log.h"
#include "expire/reaper.h"
#include "net/server.h"
#include "repl/backlog_trimmer.h"
#include "repl/replication_manager.h"
#include "script/script_engine.h"
#include "server/blocking.h"
#include "server/client_pause.h"
#include "server/compaction_scheduler.h"
#include "server/config.h"
#include "server/watch.h"
#include "storage/tides_storage.h"
#include "types/cursor_store.h"
#include "types/oid_allocator.h"

namespace tideskv {

Status SetupLogging(const ConfigValues& values);

// Owns every long-lived component and implements ServerServices for commands.
class App final : public ServerServices {
 public:
  static Result<std::unique_ptr<App>> Create(std::unique_ptr<Config> config);
  ~App() override;

  Status Start();  // bind + start threads
  void Run();      // blocks until SIGINT/SIGTERM or SHUTDOWN, then calls Stop()
  void Stop();     // idempotent
  uint16_t port() const { return server_->port(); }

  Config& config() override { return *config_; }
  const CommandRegistry& registry() const override { return registry_; }
  ServerStats Stats() const override;
  void ResetStats() override { server_->ResetCounters(); }
  void RequestShutdown() override;
  Engine& engine() override { return *engine_; }
  CursorStore& cursors() override { return cursors_; }
  OidAllocator& oids() override { return *oids_; }
  BlockingRegistry& blocking() override { return blocking_; }
  WatchRegistry& watches() override { return watches_; }
  ScriptEngine& scripts() override { return scripts_; }
  Replication& replication() override { return *replication_; }
  ClientPause& pause() override { return pause_; }
  cluster::ClusterManager* cluster() override { return cluster_.get(); }

 private:
  App(std::unique_ptr<Config> config, std::unique_ptr<TidesStorage> storage);
  net::ServerOptions MakeServerOptions();
  Status LoadReplState(repl::ReplIdentity* identity, bool* baseline_unknown);
  Status CreateCluster();  // M7a D19: before CreateReplication (the boot role comes from the topology)
  cluster::ClusterManager::Options ClusterOptions(const ConfigValues& v);
  void RefreshRedirect();        // D16, M7b D18: blocked clients follow topology and states
  bool StartsAsReplica() const;  // R12 rotates the replid of a node that starts as a master
  Status CreateReplication();
  void FillStorageStats(ServerStats& s) const;
  void FillReplStats(ServerStats& s, const ConfigValues& v) const;
  Status StartRole();  // the reaper on a master, the replica client when replicaof (or the topology) says so

  std::unique_ptr<Config> config_;
  std::unique_ptr<TidesStorage> storage_;
  std::unique_ptr<ReplLog> repl_log_;  // before oids_ and engine_: both commit through it
  std::unique_ptr<OidAllocator> oids_;
  std::unique_ptr<LockManager> locks_ = std::make_unique<LockManager>();
  SystemClock clock_;
  // After storage_ and repl_log_ (it streams the replog); created in Create() once the identity is loaded.
  // Shut down first in Stop(); kept alive until destruction so replication() stays valid.
  std::unique_ptr<repl::ReplicationManager> replication_;
  // D18: trims the replog; its thread on a master, the replica client's applier on a replica. Created
  // with replication_ (each refers to the other); stopped first in Stop(), destroyed before repl_log_.
  std::unique_ptr<repl::BacklogTrimmer> trimmer_;
  // Its hooks call replication_, which outlives it; commands reach it through server_, destroyed first.
  std::unique_ptr<cluster::ClusterManager> cluster_;
  ScriptEngine scripts_;                            // before engine_: its busy guard refers to it
  WatchRegistry watches_;                           // before engine_ and the reaper: it must outlive them
  std::unique_ptr<CompactionScheduler> scheduler_;  // compacts storage_; the engine feeds it
  std::unique_ptr<Engine> engine_;
  ClientPause pause_;  // before reaper_ (its paused hook reads it) and server_ (connections park in it)
  std::unique_ptr<ExpireReaper> reaper_;  // uses engine_; destroyed in Stop() before it; started by role
  CommandRegistry registry_;
  CursorStore cursors_;
  BlockingRegistry blocking_;  // before server_: connections cancel their waiters when destroyed
  std::unique_ptr<net::Server> server_;
  asio::io_context main_ctx_;
  std::optional<asio::signal_set> signals_;  // registered in Start(), before accepting
  std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
  bool stopped_ = false;
};

}  // namespace tideskv
