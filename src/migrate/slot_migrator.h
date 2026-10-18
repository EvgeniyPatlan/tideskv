#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "cluster/topology.h"
#include "common/status.h"
#include "encoding/replog_codec.h"
#include "migrate/migrate_settings.h"
#include "migrate/migration_transport.h"
#include "migrate/rate_limiter.h"

// The source side of one migration (M7b D2-D5, D10, D19): one thread per job. Connect + AUTH + IMPORT <R> 0,
// the range's snapshot as IMPORTDATA chunks, catch-up rounds of the replog filtered to the range, the fence
// (a states Mutate under the exclusive gate, returning its seq), the final delta through that seq, IMPORT <R>
// 1, then the outcome is published (migrated persisted only now, D8). Any failure: IMPORT <R> 2 (best
// effort), the fence lowered, the block says fail. job_mu_ is a leaf; the thread never takes apply_mu_ or
// role_mu_.
namespace tideskv::migrate {

class MigrationService;

struct MigrateJob {
  cluster::SlotRange range;
  std::string dst_id;
  std::string dst_host;  // from this node's topology at MIGRATE
  uint16_t dst_port = 0;
};

// Ok = success; UserError = the stage error. Runs on the job's thread, after the outcome is published: it
// must not call Start, Stop or WaitIdle, nor take MigrationService::migrate_mu_ (CLUSTERX MIGRATE holds it
// while it starts the job). The production one (cmd_clusterx_migrate.cc, Deliverer) takes only its own locks.
using MigrateDone = std::function<void(const Status& outcome)>;

class SlotMigrator {
 public:
  struct Hooks {                           // test seams (D25), run on the migrator thread
    std::function<void()> after_snapshot;  // the snapshot is sent, before the catch-up
    std::function<void()> before_fence;    // the instant before the barrier
    std::function<void()> after_fence;     // fenced and published, before the final delta
  };

  SlotMigrator(MigrationService& service, TransportFactory connect,
               std::function<MigrateSettings()> settings);
  ~SlotMigrator();  // Stop()
  SlotMigrator(const SlotMigrator&) = delete;
  SlotMigrator& operator=(const SlotMigrator&) = delete;

  // Runs `job` on a new thread; `done` (may be null) gets the outcome after it is published. The caller
  // published the start state first (D12). kErrAlreadyMigrating while a job runs.
  Status Start(MigrateJob job, MigrateDone done);
  void Cancel();  // D10: non-blocking; the running job ends in fail with kErrMigrationCanceled
  // Cancel, shut the link down, join; later Starts are refused. Never under apply_mu_. On the job's own
  // thread (from `done`) the thread is detached instead of joined.
  void Stop();
  bool running() const { return running_.load(std::memory_order_acquire); }
  bool WaitIdle(std::chrono::milliseconds timeout);  // true once no job runs
  uint64_t oldest_needed() const { return next_seq_.load(std::memory_order_acquire); }  // D19
  void SetHooks(Hooks hooks) { hooks_ = std::move(hooks); }                             // before Start

 private:
  void Run(const MigrateJob& job, const MigrateDone& done);
  Status Execute(const MigrateJob& job);
  Status Open(const MigrateJob& job);
  Status SendSnapshot(const MigrateJob& job, uint64_t* next);
  Status CatchUp(const MigrateJob& job, uint64_t* next);
  Status FenceAndFinish(const MigrateJob& job, uint64_t next);
  Status ShipRange(const MigrateJob& job, uint64_t* next, uint64_t last);
  Status SendOps(const std::vector<encoding::ReplOp>& ops);
  Status SendChunk(std::string_view chunk);
  Status Publish(const MigrateJob& job, const Status& outcome);
  Status PublishSuccess(const MigrateJob& job);
  void PublishFailure(const MigrateJob& job);
  void Delay();
  Status Canceled() const;

  MigrationService& service_;
  TransportFactory connect_;
  std::function<MigrateSettings()> settings_;
  Hooks hooks_;
  // The job's thread only.
  MigrateSettings current_;
  RateLimiter limiter_;
  bool import_started_ = false;
  std::unique_ptr<MigrationTransport> transport_;  // set and reset under job_mu_ (Stop shuts it down)

  mutable std::mutex job_mu_;
  std::condition_variable idle_cv_;
  std::thread thread_;  // guarded by job_mu_
  std::atomic<bool> running_{false};
  std::atomic<bool> cancel_{false};
  std::atomic<bool> stopped_{false};
  std::atomic<uint64_t> next_seq_;
};

}  // namespace tideskv::migrate
