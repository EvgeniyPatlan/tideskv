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
#include "expire/reaper.h"
#include "net/server.h"
#include "server/config.h"
#include "storage/tides_storage.h"
#include "types/cursor_store.h"

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

 private:
  App(std::unique_ptr<Config> config, std::unique_ptr<TidesStorage> storage);
  net::ServerOptions MakeServerOptions();

  std::unique_ptr<Config> config_;
  std::unique_ptr<TidesStorage> storage_;
  std::unique_ptr<LockManager> locks_ = std::make_unique<LockManager>();
  SystemClock clock_;
  std::unique_ptr<Engine> engine_;
  std::unique_ptr<ExpireReaper> reaper_;  // uses engine_; destroyed in Stop() before it
  CommandRegistry registry_;
  CursorStore cursors_;
  std::unique_ptr<net::Server> server_;
  asio::io_context main_ctx_;
  std::optional<asio::signal_set> signals_;  // registered in Start(), before accepting
  std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
  bool stopped_ = false;
};

}  // namespace tideskv
