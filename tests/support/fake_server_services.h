#pragma once
#include <stdexcept>
#include <string>
#include <vector>

#include "commands/registry.h"
#include "commands/server_services.h"
#include "exec/engine.h"
#include "exec/lock_manager.h"
#include "exec/repl_log.h"
#include "script/script_engine.h"
#include "server/blocking.h"
#include "server/watch.h"
#include "storage/memory_storage.h"
#include "support/fake_clock.h"
#include "support/faulty_storage.h"
#include "types/cursor_store.h"
#include "types/oid_allocator.h"

namespace tideskv::test {

class FakeServerServices final : public ServerServices {
 public:
  FakeServerServices() {
    if (Status st = repl_log_.Recover(); !st.ok())
      throw std::runtime_error("ReplLog::Recover: " + st.ToString());
    const Status st = oids_.Init();
    if (!st.ok()) throw std::runtime_error("OidAllocator::Init: " + st.ToString());
  }

  Config& config() override { return config_; }
  const CommandRegistry& registry() const override { return registry_; }
  ServerStats Stats() const override { return stats_; }
  void ResetStats() override { ++reset_calls_; }
  void RequestShutdown() override { shutdown_requested_ = true; }
  Engine& engine() override { return engine_; }
  CursorStore& cursors() override { return cursors_; }
  OidAllocator& oids() override { return oids_; }
  BlockingRegistry& blocking() override { return blocking_; }
  WatchRegistry& watches() override { return watches_; }
  ScriptEngine& scripts() override { return scripts_; }

  CommandRegistry registry_;
  Config config_;
  ServerStats stats_;
  int reset_calls_ = 0;
  bool shutdown_requested_ = false;
  CursorStore cursors_;
  ScriptEngine scripts_;
  FaultyStorage storage_;
  LockManager locks_;
  FakeClock clock_;
  BlockingRegistry blocking_;           // before engine_
  WatchRegistry watches_;               // before engine_
  ReplLog repl_log_{storage_, clock_};  // after storage_ and clock_
  OidAllocator oids_{storage_, {}, [this](const std::vector<encoding::ReplOp>& ops) {
                       return repl_log_.CommitStandalone(ops);
                     }};  // after storage_ and repl_log_
  // Declared after what it references.
  Engine engine_{storage_, locks_, clock_, nullptr, &watches_, &repl_log_};
};

}  // namespace tideskv::test
