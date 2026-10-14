#pragma once
#include <unistd.h>

#include <atomic>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "commands/registry.h"
#include "commands/server_services.h"
#include "common/string_util.h"
#include "exec/engine.h"
#include "exec/lock_manager.h"
#include "exec/repl_log.h"
#include "repl/replication.h"
#include "script/script_engine.h"
#include "server/blocking.h"
#include "server/watch.h"
#include "storage/memory_storage.h"
#include "support/fake_clock.h"
#include "support/faulty_storage.h"
#include "types/cursor_store.h"
#include "types/oid_allocator.h"

namespace tideskv::test {

// A Replication whose role the test sets. PSYNC is decided with the real rule (DecidePsync) over `log`;
// every call is recorded. Not thread-safe beyond role().
class FakeReplication final : public Replication {
 public:
  struct Attached {
    uint64_t pending_id = 0;
    std::string unread;
  };

  ReplRole role() const override { return role_.load(); }
  void set_role(ReplRole r) { role_.store(r); }
  PsyncResult HandlePsync(const repl::PsyncRequest& req, const ReplicaPeer& peer) override {
    psyncs.push_back(req);
    last_peer = peer;
    if (!psync_error.empty()) {
      PsyncResult refused;
      refused.error = psync_error;
      return refused;
    }
    const repl::LogBounds bounds{log->first_retained(), log->last()};
    PsyncResult r{repl::DecidePsync(identity, bounds, req), identity.replid, bounds.last, 0, {}};
    if (r.decision == repl::PsyncDecision::kContinue) r.pending_id = last_pending_id = ++next_pending_id_;
    return r;
  }
  void AttachReplica(uint64_t pending_id, int fd, std::string unread) override {
    ::close(fd);
    attached.push_back({pending_id, std::move(unread)});
  }
  void OnReplconf(Session&, const ReplconfOptions& o) override { replconfs.push_back(o); }
  bool BecomeReplica(const std::string& host, uint16_t port) override {
    if (role() == ReplRole::kReplica && EqualsIgnoreCase(host, role_info.host) && port == role_info.port) {
      return false;
    }
    become_replica.emplace_back(host, port);
    role_info = RoleInfo{ReplRole::kReplica, host, port, ReplicaLinkState::kConnect, 0, -1, {}};
    set_role(ReplRole::kReplica);
    return true;
  }
  Status BecomeMaster() override {
    ++become_master;
    role_info = RoleInfo{};
    set_role(ReplRole::kMaster);
    return Status::Ok();
  }
  RoleInfo Role() const override {
    RoleInfo r = role_info;
    r.role = role();
    r.offset = log->last();
    return r;
  }

  std::atomic<ReplRole> role_{ReplRole::kMaster};
  repl::ReplIdentity identity{std::string(repl::kReplIdLength, 'c'), {}};
  const ReplLog* log = nullptr;  // set by FakeServerServices
  std::vector<repl::PsyncRequest> psyncs;
  std::string psync_error;  // non-empty: HandlePsync refuses with it
  ReplicaPeer last_peer;
  uint64_t last_pending_id = 0;
  std::vector<Attached> attached;
  std::vector<ReplconfOptions> replconfs;
  RoleInfo role_info;  // what Role() reports (role and offset are filled in live)
  std::vector<std::pair<std::string, uint16_t>> become_replica;
  int become_master = 0;

 private:
  uint64_t next_pending_id_ = 0;
};

class FakeServerServices final : public ServerServices {
 public:
  FakeServerServices() {
    replication_.log = &repl_log_;
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
  Replication& replication() override {
    return real_replication != nullptr ? *real_replication : replication_;
  }

  CommandRegistry registry_;
  Config config_;
  ServerStats stats_;
  int reset_calls_ = 0;
  bool shutdown_requested_ = false;
  CursorStore cursors_;
  ScriptEngine scripts_;
  FakeReplication replication_;
  Replication* real_replication = nullptr;  // set: replication() returns it instead of the fake
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
