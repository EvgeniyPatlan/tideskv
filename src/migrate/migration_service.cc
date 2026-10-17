#include "migrate/migration_service.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <exception>
#include <optional>
#include <utility>

#include "migrate/slot_eraser.h"

namespace tideskv::migrate {

MigrationService::MigrationService(MigrationDeps deps)
    : deps_(std::move(deps)), worker_([this] { WorkerLoop(); }) {}

MigrationService::~MigrationService() { Shutdown(); }

Status MigrationService::SaveRecord(const cluster::MigrationRecord& record) {
  if (Status st = deps_.log.CommitStandalone({cluster::MigrationRecordPut(record)}); !st.ok()) return st;
  return deps_.make_durable ? deps_.make_durable() : Status::Ok();
}

Status MigrationService::Mutate(const FallibleEdit& edit, bool persist, uint64_t* fence_seq) {
  std::lock_guard lock(record_mu_);
  const std::shared_ptr<const cluster::SlotStates> cur = deps_.cluster.States();
  auto next = std::make_shared<cluster::SlotStates>(*cur);
  const bool restoring = restore_pending_.load(std::memory_order_acquire);
  cluster::MigrationRecord stored = cluster::RecordOf(*cur);  // what sys/migration holds now
  std::vector<cluster::SlotRange> resumed;
  if (restoring) {
    if (Status st = RestoreInto(*next, &stored, &resumed); !st.ok()) return st;
  }
  if (Status st = edit(*next); !st.ok()) return st;
  if (Status st = Commit(cur->fenced, stored, std::move(next), persist, fence_seq); !st.ok()) return st;
  if (restoring) {
    restore_pending_.store(false, std::memory_order_release);
    for (const cluster::SlotRange& r : resumed) Post([this, r] { RunCleanup(r); });
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

Status MigrationService::RestoreInto(cluster::SlotStates& s, cluster::MigrationRecord* stored,
                                     std::vector<cluster::SlotRange>* resumed) {
  auto loaded = cluster::LoadMigrationRecord(deps_.storage);
  if (!loaded.ok()) return loaded.status();
  const std::optional<cluster::MigrationRecord>& record = *loaded;
  cluster::SlotStates restored =
      record.has_value() ? cluster::StatesFromRecord(*record) : cluster::SlotStates{};
  if (restored.import && restored.import->state == cluster::ImportState::kStart) {  // a half import (D8)
    if (Status st = EraseWaitingOutBusy(restored.import->range, /*background=*/false); !st.ok()) return st;
    restored.import->state = cluster::ImportState::kError;
  }
  *stored = record.value_or(cluster::MigrationRecord{});
  *resumed = restored.cleanup;
  s = std::move(restored);
  if (record.has_value()) spdlog::info("slot migration states restored from sys/migration");
  return Status::Ok();
}

Status MigrationService::Restore() {
  restore_pending_.store(true, std::memory_order_release);
  return Mutate([](cluster::SlotStates&) {});
}

void MigrationService::OnPromoted() {
  restore_pending_.store(true, std::memory_order_release);
  Post([this] {
    if (!restore_pending_.load(std::memory_order_acquire)) return;  // an earlier Mutate restored already
    const Status st = Mutate([](cluster::SlotStates&) {});
    if (st.ok()) return;
    spdlog::critical("cannot restore the slot migration states after a promotion: {}", st.ToString());
    // Writer's note 6: a damaged record leaves the states empty; any other failure retries on the next
    // Mutate.
    if (st.code() == StatusCode::kCorruption) restore_pending_.store(false, std::memory_order_release);
  });
}

Status MigrationService::EraseRange(cluster::SlotRange range, bool background) {
  SlotEraser::Hooks hooks;
  hooks.stopping = [this] { return stopping(); };
  if (background && deps_.begin_background_write) {
    hooks.begin_write = deps_.begin_background_write;
    hooks.end_write = deps_.end_background_write;
  }
  return SlotEraser(deps_.engine, std::move(hooks)).Erase(range);
}

Status MigrationService::EraseWaitingOutBusy(cluster::SlotRange range, bool background) {
  for (;;) {
    Status st = EraseRange(range, background);
    if (!deps_.engine.IsBusyAnswer(st) || stopping()) return st;
    WaitBeforeRetry(kBusyRetry);  // a timed-out script holds the gate: not a failure
  }
}

void MigrationService::RunCleanup(cluster::SlotRange range) {
  if (stopping() || deps_.is_replica()) return;
  Status st = EraseWaitingOutBusy(range, /*background=*/true);
  if (st.ok()) {
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
  {
    std::lock_guard lock(worker_mu_);
    stop_.store(true, std::memory_order_release);
  }
  worker_cv_.notify_all();
  if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
}

}  // namespace tideskv::migrate
