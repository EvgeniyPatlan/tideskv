#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "cluster/cluster_manager.h"
#include "cluster/migration_record.h"
#include "cluster/slot_states.h"
#include "common/status.h"
#include "exec/engine.h"
#include "exec/repl_log.h"
#include "migrate/migrate_settings.h"
#include "migrate/migration_transport.h"
#include "migrate/slot_importer.h"
#include "migrate/slot_migrator.h"
#include "storage/storage.h"
#include "types/oid_allocator.h"

// Slot migration on one node (M7b): the states it publishes next to the topology, the replicated
// sys/migration record, the migrate worker thread (cleanups, link-loss erases, SETNODES transitions) and the
// target's importer (Task 9) and the source's migrator (Task 10). Owned by App when cluster-enabled;
// commands reach it only through ServerServices::migration() (D26). Lock order: migrate_mu_ -> apply_mu_
// (ResetCluster only; nothing under apply_mu_ takes migrate_mu_); {migrate_mu_ |
// SlotImporter::mu_} -> record_mu_ -> Engine gate (taken and released) -> (states publish, never under the
// gate); worker_mu_ is a leaf.
namespace tideskv::migrate {

struct MigrationDeps {
  Engine& engine;
  ReplLog& log;
  Storage& storage;
  OidAllocator& oids;
  cluster::ClusterManager& cluster;
  std::function<bool()> is_replica = {};              // required
  std::function<bool()> begin_background_write = {};  // CLIENT PAUSE (D9); empty = never paused
  std::function<void()> end_background_write = {};
  // After each sys/migration write, as for sys/cluster_cfg (M7a D20; App: storage Checkpoint), so a kill -9
  // right after a success or an import keeps the outcome (D8, Writer's note 8). Empty = none.
  std::function<Status()> make_durable = {};
  TransportFactory connect = {};                   // required from Task 10: the session with a target (D3)
  std::function<MigrateSettings()> settings = {};  // required from Task 10: re-read before every chunk
};

class MigrationService {
 public:
  using Edit = std::function<void(cluster::SlotStates&)>;
  using FallibleEdit = std::function<Status(cluster::SlotStates&)>;  // a failure changes nothing

  explicit MigrationService(MigrationDeps deps);
  ~MigrationService();  // Shutdown()
  MigrationService(const MigrationService&) = delete;
  MigrationService& operator=(const MigrationService&) = delete;

  // Read-modify-write of the published states under record_mu_: `edit` changes a copy; the Engine fence moves
  // to the copy's fenced set (*fence_seq gets the barrier's seq when it moved); sys/migration is written when
  // its persisted part changed (persist, and only on a master); then the copy is published, after the gate is
  // released. On a failure (the edit's, the write's) the fence moves back and nothing is published. While a
  // promotion's restore is pending, the copy is first rebuilt from the record (cheaply: no erase here, see
  // Restore), so an edit that lands before the posted restore neither overwrites the record nor is
  // overwritten by it. On a replica it is skipped; a damaged record is logged at critical and leaves empty
  // states (Writer's note 6); a transient load error fails the edit and the restore stays pending.
  Status Mutate(const FallibleEdit& edit, bool persist = true, uint64_t* fence_seq = nullptr) {
    return MutateImpl(edit, persist, fence_seq, /*strict_restore=*/false);
  }
  template <class F>
    requires std::is_void_v<std::invoke_result_t<F&, cluster::SlotStates&>>
  Status Mutate(F&& edit, bool persist = true, uint64_t* fence_seq = nullptr) {
    return Mutate(FallibleEdit([&edit](cluster::SlotStates& s) {
                    edit(s);
                    return Status::Ok();
                  }),
                  persist, fence_seq);
  }
  // D8, on a master at boot, as one Mutate: the record's states (nothing when absent); an import left in
  // start becomes error and its erase is posted to the worker (with the CLIENT PAUSE hooks), as is each
  // cleanup-pending range. Corruption for a damaged record (the boot stops). Then OnTopology (ruling S4): a
  // migrated range the persisted topology already moved away is cleaned up.
  Status Restore();
  // The become_master hook (under apply_mu_): requests the restore and posts it; the first Mutate after it,
  // on any thread, performs it (see Mutate). A second promotion before that is never lost (a generation).
  void OnPromoted();
  // D9, on the worker: erases `range`, then drops its cleanup entry, its migrated entry and its fence
  // (persisted). BUSY is waited out; another failure is retried after 1 s until Shutdown; a replica does
  // nothing (its master resumes).
  void RunCleanup(cluster::SlotRange range);
  // The eraser stopping on Shutdown, with the CLIENT PAUSE hooks (background) or without (inside an admitted
  // command, M7a D21). One attempt: BUSY is returned.
  Status EraseRange(cluster::SlotRange range, bool background);
  // EraseRange until it is not BUSY or Shutdown (the worker's erases); `stop` (optional) also stops the
  // eraser before a txn.
  Status EraseWaitingOutBusy(cluster::SlotRange range, bool background,
                             const std::function<bool()>& stop = {});
  SlotImporter& importer() { return *importer_; }                       // the target side (M7b D14)
  SlotMigrator& migrator() { return *migrator_; }                       // the source side (M7b D2-D5)
  uint64_t OldestNeeded() const { return migrator_->oldest_needed(); }  // D19: App pins the backlog with it

  // ---- Control, from the commands (Task 11) ----
  // CLUSTERX MIGRATE (D12): one range; Kvrocks's execute checks in order; the start state published before
  // returning; `done` (may be null) gets the outcome. Serialised by migrate_mu_.
  Status Migrate(const std::vector<cluster::SlotRange>& ranges, std::string_view dst_id, MigrateDone done);
  // FLUSHDB/FLUSHALL (D10, Writer's note 1): cancels a running job and fails a running import. Flags and a
  // post only: it may run inside an EXEC or a script that holds the Engine gate.
  void OnFlush();
  // D13: refused while a job is in start; the check and the erase run under migrate_mu_, so no MIGRATE starts
  // in between.
  Status FlushSlots(const std::vector<cluster::SlotRange>& ranges);
  bool MigrationRunning() const;  // a job in start (CLUSTER RESET, FLUSHSLOTS)
  bool ImportRunning() const;     // an import in start (CLUSTER RESET)
  // CLUSTER RESET succeeded (D15, D20): migrated, cleanup and imported ranges and their fences go; a success
  // block goes, a fail block and the import block stay.
  Status OnReset();
  // CLUSTER RESET (D20), all under migrate_mu_ so no MIGRATE publishes start in between: refused while a job
  // or an import is in start (Kvrocks order); then every slot held fenced (Engine::HoldAllFenced: no
  // concurrent Mutate can lower it) while `reset` (the emptiness check and ClusterManager::Reset, which takes
  // apply_mu_: migrate_mu_ -> apply_mu_) and, on success, OnReset run; the release puts the fence back to the
  // states' set. Never under apply_mu_ or the Engine gate.
  Status ResetCluster(const std::function<Status()>& reset);

  // ---- SETNODES transitions (Task 12, D7) ----
  // The on_transition hook (under apply_mu_): posts D7's table to the worker, which reads the topology
  // published by then. Never takes migrate_mu_.
  void OnTopology();
  // The stop_migration hook (under apply_mu_): cancels the job (a flag) and fails a running import; the
  // worker clears the states (a replica holds none, D21; a later promotion restores them from the record).
  void OnDemoted();

  void Post(std::function<void()> task);              // the migrate worker, FIFO; dropped after Shutdown
  bool AwaitIdle(std::chrono::milliseconds timeout);  // true once every task posted before the call ran
  void Shutdown();                                    // idempotent: stops the worker
  bool stopping() const { return stop_.load(std::memory_order_acquire); }
  const MigrationDeps& deps() const { return deps_; }

 private:
  static constexpr std::chrono::milliseconds kBusyRetry{10};  // as the eraser polls CLIENT PAUSE
  static constexpr std::chrono::seconds kFailureRetry{1};

  struct Restored {  // what a restore leaves for the worker once its Mutate committed
    bool ran = false;
    std::vector<cluster::SlotRange> cleanup;
    std::optional<cluster::SlotRange> half_import;
    std::vector<cluster::SlotRange> imported;  // the record's imported ranges
  };

  Status MutateImpl(const FallibleEdit& edit, bool persist, uint64_t* fence_seq, bool strict_restore);
  bool RestorePending() const {
    return restore_requested_.load(std::memory_order_acquire) !=
           restore_applied_.load(std::memory_order_acquire);
  }
  // Under record_mu_, for generation `wanted`: skipped on a replica; a failure is returned when strict or not
  // Corruption (the restore stays pending); a Corruption otherwise is logged at critical with `s` left empty.
  // Marks `wanted` applied unless it rebuilt `s` (`out->ran`: the caller marks it once its commit succeeded)
  // or failed strictly.
  Status PrepareRestore(uint64_t wanted, cluster::SlotStates& s, cluster::MigrationRecord* stored,
                        Restored* out, bool strict);
  // Under record_mu_: `s` becomes the record's states, a half import -> error; `stored` gets the record as
  // found (empty when absent).
  Status RestoreInto(cluster::SlotStates& s, cluster::MigrationRecord* stored, Restored* out);
  void PostRestored(const Restored& r);  // the cleanups and the half-import erase, to the worker
  // On the worker: erases what an import left in start (D8). Stops for good once any other import overlaps it
  // (start, success or error: that import erased its range itself, D9), or a range imported since the restore
  // (not in `restored_imported`) does, on a replica, or on Shutdown; failures retry after 1 s.
  void RunHalfImportErase(cluster::SlotRange range, std::vector<cluster::SlotRange> restored_imported);
  bool NewImportOverlaps(cluster::SlotRange range,
                         const std::vector<cluster::SlotRange>& restored_imported) const;
  // Under record_mu_: moves the fence from `cur_fenced`, writes RecordOf(next) unless it equals `stored`,
  // publishes `next`.
  Status Commit(const SlotBits& cur_fenced, const cluster::MigrationRecord& stored,
                std::shared_ptr<cluster::SlotStates> next, bool persist, uint64_t* fence_seq);
  Status SaveRecord(const cluster::MigrationRecord& record);
  Status EraseWith(cluster::SlotRange range, bool background,
                   std::function<bool()> stop);  // EraseRange + stop
  void WorkerLoop();
  void WaitBeforeRetry(std::chrono::milliseconds delay);  // cut short by Shutdown
  // On the worker (migration_transitions.cc): D7's table against the topology published now, inside one
  // Mutate (ruling S2(a)); then the cleanups it scheduled run inline, so CLUSTERX can wait for them.
  void ApplyTransition();

  MigrationDeps deps_;
  std::mutex record_mu_;
  std::mutex migrate_mu_;  // Migrate, FlushSlots, ResetCluster: each one's checks and its change, together
  // Restore generations: Restore/OnPromoted bump requested; a restore (or its skip) marks it applied, under
  // record_mu_. Pending while they differ.
  std::atomic<uint64_t> restore_requested_{0};
  std::atomic<uint64_t> restore_applied_{0};
  std::atomic<bool> stop_{false};
  std::mutex worker_mu_;
  std::condition_variable worker_cv_;
  std::deque<std::function<void()>> tasks_;  // guarded by worker_mu_
  uint64_t posted_ = 0;                      // guarded by worker_mu_
  uint64_t done_ = 0;                        // guarded by worker_mu_
  std::unique_ptr<SlotMigrator> migrator_;   // stopped first by Shutdown
  std::unique_ptr<SlotImporter> importer_;   // its worker tasks end before Shutdown returns
  std::thread worker_;                       // last: starts once every other member exists
};

}  // namespace tideskv::migrate
