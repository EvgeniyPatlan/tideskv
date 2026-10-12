#pragma once
#include "commands/registry.h"
#include "commands/server_services.h"
#include "exec/engine.h"
#include "exec/lock_manager.h"
#include "storage/memory_storage.h"
#include "support/fake_clock.h"
#include "types/cursor_store.h"

namespace tideskv::test {

class FakeServerServices final : public ServerServices {
 public:
  Config& config() override { return config_; }
  const CommandRegistry& registry() const override { return registry_; }
  ServerStats Stats() const override { return stats_; }
  void ResetStats() override { ++reset_calls_; }
  void RequestShutdown() override { shutdown_requested_ = true; }
  Engine& engine() override { return engine_; }
  CursorStore& cursors() override { return cursors_; }

  CommandRegistry registry_;
  Config config_;
  ServerStats stats_;
  int reset_calls_ = 0;
  bool shutdown_requested_ = false;
  CursorStore cursors_;
  InMemoryStorage storage_;
  LockManager locks_;
  FakeClock clock_;
  Engine engine_{storage_, locks_, clock_};  // declared after what it references
};

}  // namespace tideskv::test
