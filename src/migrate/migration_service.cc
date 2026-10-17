#include "migrate/migration_service.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <exception>
#include <optional>
#include <thread>
#include <utility>

#include "migrate/slot_eraser.h"

namespace tideskv::migrate {

MigrationService::MigrationService(MigrationDeps deps)
    : deps_(std::move(deps)),
      migrator_(std::make_unique<SlotMigrator>(*this, deps_.connect, deps_.settings)),
      importer_(std::make_unique<SlotImporter>(*this)),
      worker_([this] { WorkerLoop(); }) {}

MigrationService::~MigrationService() { Shutdown(); }

Status MigrationService::SaveRecord(const cluster::MigrationRecord& record) {
  if (Status st = deps_.log.CommitStandalone({cluster::MigrationRecordPut(record)}); !st.ok()) return st;
  return deps_.make_durable ? deps_.make_durable() : Status::Ok();
}

Status MigrationService::MutateImpl(const FallibleEdit& edit, bool persist, uint64_t* fence_seq,
                                    bool strict_restore) {
  std::lock_guard lock(record_mu_);
  const std::shared_ptr<const cluster::SlotStates> cur = deps_.cluster.States();
  auto next = std::make_shared<cluster::SlotStates>(*cur);
  cluster::MigrationRecord stored = cluster::RecordOf(*cur);  // what sys/migration holds now
  const uint64_t wanted = restore_requested_.load(std::memory_order_acquire);
  Restored restored;
  if (wanted != restore_applied_.load(std::memory_order_acquire)) {
    if (Status st = PrepareRestore(wanted, *next, &stored, &restored, strict_restore); !st.ok()) return st;
  }
  if (Status st = edit(*next); !st.ok()) return st;
  if (Status st = Commit(cur->fenced, stored, std::move(next), persist, fence_seq); !st.ok()) return st;
  if (restored.ran) {
    restore_applied_.store(wanted, std::memory_order_release);
    PostRestored(restored);
  }
  return Status::Ok();
}

Status MigrationService::Commit(const SlotBits& cur_fenced, const cluster::MigrationRecord& stored,
                                std::shared_ptr<cluster::SlotStates> next, bool persist,
                                uint64_t* fence_seq) {
  const bool refence = next->fenced != cur_fenced;
  if (refence) {  // takes and releases the exclusive gate: the publish below runs outside it
    const uint64_t seq = deps_.engine.AssignFence(next->fenced);
    if (fence_seq != nullptr) *fence_seq = seq;
  }
  if (persist && !deps_.is_replica()) {  // a replica owns no migration outcome (D21)
    const cluster::MigrationRecord record = cluster::RecordOf(*next);
    if (!(record == stored)) {
      if (Status st = SaveRecord(record); !st.ok()) {
        if (refence) deps_.engine.AssignFence(cur_fenced);
        return st;
      }
    }
  }
  deps_.cluster.PublishStates(std::move(next));
  return Status::Ok();
}

Status MigrationService::PrepareRestore(uint64_t wanted, cluster::SlotStates& s,
                                        cluster::MigrationRecord* stored, Restored* out, bool strict) {
  if (deps_.is_replica()) {  // demoted again before the restore: a replica holds the record passively (D21)
    restore_applied_.store(wanted, std::memory_order_release);
    return Status::Ok();
  }
  Status st = RestoreInto(s, stored, out);
  // A transient load error fails this edit and leaves the restore pending: empty states would let the next
  // persisting edit overwrite a still-valid record (controller ruling, fix round 2).
  if (st.ok() || strict || st.code() != StatusCode::kCorruption) return st;
  // Writer's note 6: a damaged record leaves empty states; it never fails later edits.
  spdlog::critical("cannot restore the slot migration states after a promotion: {}", st.ToString());
  s = cluster::SlotStates{};
  *stored = cluster::MigrationRecord{};
  restore_applied_.store(wanted, std::memory_order_release);
  return Status::Ok();
}

Status MigrationService::RestoreInto(cluster::SlotStates& s, cluster::MigrationRecord* stored,
                                     Restored* out) {
  auto loaded = cluster::LoadMigrationRecord(deps_.storage);
  if (!loaded.ok()) return loaded.status();
  const std::optional<cluster::MigrationRecord>& record = *loaded;
  cluster::SlotStates restored =
      record.has_value() ? cluster::StatesFromRecord(*record) : cluster::SlotStates{};
  if (restored.import && restored.import->state == cluster::ImportState::kStart) {  // a half import (D8)
    out->half_import = restored.import->range;  // erased on the worker once this commits
    restored.import->state = cluster::ImportState::kError;
  }
  *stored = record.value_or(cluster::MigrationRecord{});
  out->cleanup = restored.cleanup;
  out->imported = restored.imported;
  out->ran = true;
  s = std::move(restored);
  if (record.has_value()) spdlog::info("slot migration states restored from sys/migration");
  return Status::Ok();
}

void MigrationService::PostRestored(const Restored& r) {
  if (r.half_import) {
    Post([this, range = *r.half_import, imported = r.imported] { RunHalfImportErase(range, imported); });
  }
  for (const cluster::SlotRange& c : r.cleanup) Post([this, c] { RunCleanup(c); });
}

Status MigrationService::Restore() {
  restore_requested_.fetch_add(1, std::memory_order_acq_rel);
  Status st = MutateImpl([](cluster::SlotStates&) { return Status::Ok(); }, true, nullptr,
                         /*strict_restore=*/true);
  if (st.ok()) OnTopology();  // S4: after the restored cleanups, which it never schedules twice
  return st;
}

void MigrationService::OnPromoted() {
  restore_requested_.fetch_add(1, std::memory_order_acq_rel);
  Post([this] {
    if (!RestorePending()) return;  // an earlier Mutate restored already
    if (Status st = Mutate([](cluster::SlotStates&) {}); !st.ok()) {
      spdlog::error("cannot commit the slot migration states restored after a promotion: {}", st.ToString());
    }
  });
}

bool MigrationService::NewImportOverlaps(cluster::SlotRange range,
                                         const std::vector<cluster::SlotRange>& restored_imported) const {
  const std::shared_ptr<const cluster::SlotStates> s = deps_.cluster.States();
  const std::optional<cluster::ImportJob>& import = s->import;
  const cluster::ImportJob restored{range, cluster::ImportState::kError};  // what the restore left
  if (import.has_value() && !(*import == restored) && cluster::Overlaps(import->range, range)) return true;
  // A later import reached success here and was then replaced by a disjoint one (controller ruling, Task 9).
  return std::any_of(s->imported.begin(), s->imported.end(), [&](cluster::SlotRange r) {
    return cluster::Overlaps(r, range) &&
           std::find(restored_imported.begin(), restored_imported.end(), r) == restored_imported.end();
  });
}

void MigrationService::RunHalfImportErase(cluster::SlotRange range,
                                          std::vector<cluster::SlotRange> restored_imported) {
  const auto superseded = [this, range, &restored_imported] {
    return NewImportOverlaps(range, restored_imported);
  };
  if (stopping() || deps_.is_replica() || superseded()) return;
  const Status st = EraseWaitingOutBusy(range, /*background=*/true, superseded);
  if (st.ok() || stopping() || superseded()) return;
  spdlog::warn("erasing the half import of slot(s) {} failed: {}; retrying", cluster::FormatRange(range),
               st.ToString());
  WaitBeforeRetry(kFailureRetry);
  Post([this, range, imported = std::move(restored_imported)] { RunHalfImportErase(range, imported); });
}

Status MigrationService::EraseRange(cluster::SlotRange range, bool background) {
  return EraseWith(range, background, {});
}

Status MigrationService::EraseWith(cluster::SlotRange range, bool background, std::function<bool()> stop) {
  SlotEraser::Hooks hooks;
  hooks.stopping = [this, stop = std::move(stop)] { return stopping() || (stop && stop()); };
  if (background && deps_.begin_background_write) {
    hooks.begin_write = deps_.begin_background_write;
    hooks.end_write = deps_.end_background_write;
  }
  return SlotEraser(deps_.engine, std::move(hooks)).Erase(range);
}

Status MigrationService::EraseWaitingOutBusy(cluster::SlotRange range, bool background,
                                             const std::function<bool()>& stop) {
  for (;;) {
    Status st = EraseWith(range, background, stop);
    if (!deps_.engine.IsBusyAnswer(st) || stopping()) return st;
    WaitBeforeRetry(kBusyRetry);  // a timed-out script holds the gate: not a failure
  }
}

bool MigrationService::AdmitBackgroundWrite(const std::function<bool()>& stop) {
  if (!deps_.begin_background_write) return true;
  for (;;) {  // Shutdown and `stop` end an actual wait only: unpaused, the write is admitted at once
    if (deps_.begin_background_write()) return true;
    if (stopping() || (stop && stop())) return false;
    std::this_thread::sleep_for(kBusyRetry);  // as the eraser polls CLIENT PAUSE
  }
}

void MigrationService::EndBackgroundWrite() {
  if (deps_.begin_background_write && deps_.end_background_write) deps_.end_background_write();
}

void MigrationService::RunCleanup(cluster::SlotRange range) {
  if (stopping() || deps_.is_replica()) return;
  Status st = EraseWaitingOutBusy(range, /*background=*/true);
  if (st.ok()) {
    const BackgroundWrite admitted(*this, deps_.is_replica);
    // Shutdown while paused: the record still says cleanup pending. Demoted meanwhile: its master resumes.
    if (!admitted.admitted() || deps_.is_replica()) return;
    st = Mutate([range](cluster::SlotStates& s) {
      std::erase_if(s.cleanup, [range](cluster::SlotRange c) { return c == range; });
      std::erase_if(s.migrated, [range](const cluster::MigratedRange& m) { return m.range == range; });
      cluster::SetFenced(s, range, false);
    });
  }
  if (st.ok() || stopping()) return;
  spdlog::warn("cleanup of slot(s) {} failed: {}; retrying", cluster::FormatRange(range), st.ToString());
  WaitBeforeRetry(kFailureRetry);
  Post([this, range] { RunCleanup(range); });
}

void MigrationService::WaitBeforeRetry(std::chrono::milliseconds delay) {
  std::unique_lock lock(worker_mu_);
  worker_cv_.wait_for(lock, delay, [this] { return stopping(); });
}

void MigrationService::Post(std::function<void()> task) {
  {
    std::lock_guard lock(worker_mu_);
    if (stopping()) return;
    tasks_.push_back(std::move(task));
    ++posted_;
  }
  worker_cv_.notify_all();
}

bool MigrationService::AwaitIdle(std::chrono::milliseconds timeout) {
  std::unique_lock lock(worker_mu_);
  const uint64_t target = posted_;
  return worker_cv_.wait_for(lock, timeout,
                             [&] { return done_ >= target || (stopping() && tasks_.empty()); });
}

void MigrationService::WorkerLoop() {
  std::unique_lock lock(worker_mu_);
  for (;;) {
    worker_cv_.wait(lock, [this] { return stopping() || !tasks_.empty(); });
    if (tasks_.empty()) return;  // stopping, and nothing left
    std::function<void()> task = std::move(tasks_.front());
    tasks_.pop_front();
    lock.unlock();
    try {  // an exception escaping this thread would std::terminate the server
      task();
    } catch (const std::exception& e) {
      spdlog::error("migrate worker: {}", e.what());
    }
    lock.lock();
    ++done_;
    worker_cv_.notify_all();
  }
}

void MigrationService::Shutdown() {
  migrator_->Stop();  // D10: shutdown cancels the job; the target sees the link close
  {
    std::lock_guard lock(worker_mu_);
    stop_.store(true, std::memory_order_release);
  }
  worker_cv_.notify_all();
  if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
}

}  // namespace tideskv::migrate
