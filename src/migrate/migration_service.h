#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
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
#include "storage/storage.h"
#include "types/oid_allocator.h"

// Slot migration on one node (M7b): the states it publishes next to the topology, the replicated
// sys/migration record, and the migrate worker thread (cleanups, link-loss erases, SETNODES transitions).
// Tasks 9 and 10 add the target's importer and the source's migrator. Owned by App when cluster-enabled;
// commands reach it only through ServerServices::migration() (D26). Lock order: {migrate_mu_ |
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
  // restore is pending (Restore, OnPromoted), the copy is first rebuilt from the record (see Restore), so an
  // edit that lands before the posted restore neither overwrites the record nor is overwritten by it.
  Status Mutate(const FallibleEdit& edit, bool persist = true, uint64_t* fence_seq = nullptr);
  template <class F>
    requires std::is_void_v<std::invoke_result_t<F&, cluster::SlotStates&>>
  Status Mutate(F&& edit, bool persist = true, uint64_t* fence_seq = nullptr) {
    return Mutate(FallibleEdit([&edit](cluster::SlotStates& s) {
                    edit(s);
                    return Status::Ok();
                  }),
                  persist, fence_seq);
  }
  // D8, on a master at boot or promotion, as one Mutate: the record's states (nothing when absent); an import
  // left in start is erased and becomes error; each cleanup-pending range is posted to the worker. Corruption
  // for a damaged record.
  Status Restore();
  // The become_master hook (under apply_mu_): marks the restore pending and posts it. A failure is logged at
  // critical; for a damaged record the node keeps its (empty) states (Writer's note 6).
  void OnPromoted();
  // D9, on the worker: erases `range`, then drops its cleanup entry, its migrated entry and its fence
  // (persisted). BUSY is waited out; another failure is retried after 1 s until Shutdown; a replica does
  // nothing (its master resumes).
  void RunCleanup(cluster::SlotRange range);
  // The eraser stopping on Shutdown, with the CLIENT PAUSE hooks (background) or without (inside an admitted
  // command, M7a D21). One attempt: BUSY is returned.
  Status EraseRange(cluster::SlotRange range, bool background);

  void Post(std::function<void()> task);              // the migrate worker, FIFO; dropped after Shutdown
  bool AwaitIdle(std::chrono::milliseconds timeout);  // true once every task posted before the call ran
  void Shutdown();                                    // idempotent: stops the worker
  bool stopping() const { return stop_.load(std::memory_order_acquire); }
  const MigrationDeps& deps() const { return deps_; }

 private:
  static constexpr std::chrono::milliseconds kBusyRetry{10};  // as the eraser polls CLIENT PAUSE
  static constexpr std::chrono::seconds kFailureRetry{1};

  // Under record_mu_: `s` becomes the record's states, a half import erased (-> error); `stored` gets the
  // record as found (empty when absent), `resumed` the cleanup-pending ranges.
  Status RestoreInto(cluster::SlotStates& s, cluster::MigrationRecord* stored,
                     std::vector<cluster::SlotRange>* resumed);
  // Under record_mu_: moves the fence from `cur_fenced`, writes RecordOf(next) unless it equals `stored`,
  // publishes `next`.
  Status Commit(const SlotBits& cur_fenced, const cluster::MigrationRecord& stored,
                std::shared_ptr<cluster::SlotStates> next, bool persist, uint64_t* fence_seq);
  Status SaveRecord(const cluster::MigrationRecord& record);
  Status EraseWaitingOutBusy(cluster::SlotRange range, bool background);  // until not BUSY or Shutdown
  void WorkerLoop();
  void WaitBeforeRetry(std::chrono::milliseconds delay);  // cut short by Shutdown

  MigrationDeps deps_;
  std::mutex record_mu_;
  std::atomic<bool> restore_pending_{false};  // set by Restore/OnPromoted, cleared by a Mutate that restored
  std::atomic<bool> stop_{false};
  std::mutex worker_mu_;
  std::condition_variable worker_cv_;
  std::deque<std::function<void()>> tasks_;  // guarded by worker_mu_
  uint64_t posted_ = 0;                      // guarded by worker_mu_
  uint64_t done_ = 0;                        // guarded by worker_mu_
  std::thread worker_;                       // last: starts once every other member exists
};

}  // namespace tideskv::migrate
