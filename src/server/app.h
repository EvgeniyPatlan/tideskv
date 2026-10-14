#pragma once
#include <asio.hpp>
#include <chrono>
#include <memory>
#include <optional>

#include "commands/registry.h"
#include "commands/server_services.h"
#include "common/clock.h"
#include "exec/engine.h"
#include "exec/lock_manager.h"
#include "exec/repl_log.h"
#include "expire/reaper.h"
#include "net/server.h"
#include "script/script_engine.h"
#include "server/blocking.h"
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

 private:
  App(std::unique_ptr<Config> config, std::unique_ptr<TidesStorage> storage);
  net::ServerOptions MakeServerOptions();

  std::unique_ptr<Config> config_;
  std::unique_ptr<TidesStorage> storage_;
  std::unique_ptr<ReplLog> repl_log_;  // before oids_ and engine_: both commit through it
  std::unique_ptr<OidAllocator> oids_;
  std::unique_ptr<LockManager> locks_ = std::make_unique<LockManager>();
  SystemClock clock_;
  ScriptEngine scripts_;                            // before engine_: its busy guard refers to it
  WatchRegistry watches_;                           // before engine_ and the reaper: it must outlive them
  std::unique_ptr<CompactionScheduler> scheduler_;  // compacts storage_; the engine feeds it
  std::unique_ptr<Engine> engine_;
  std::unique_ptr<ExpireReaper> reaper_;  // uses engine_; destroyed in Stop() before it
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
