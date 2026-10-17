#include "exec/engine.h"

#include <spdlog/spdlog.h>

#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

#include "encoding/replog_codec.h"
#include "exec/engine_internal.h"
#include "exec/recording_txn.h"
#include "types/errors.h"

namespace tideskv {

using engine_internal::ModifiedBy;
using engine_internal::ReentryCheck;
using engine_internal::WriteScope;

Engine::Batch* Engine::ActiveBatch() const {
  return engine_internal::t_batch != nullptr && &engine_internal::t_batch->engine_ == this
             ? engine_internal::t_batch
             : nullptr;
}

void Engine::SetBusyGuard(std::function<bool()> busy, std::string busy_reply) {
  busy_ = std::move(busy);
  busy_reply_ = std::move(busy_reply);
}

Engine::WaitThroughBusyScope::WaitThroughBusyScope()
    : previous_(std::exchange(engine_internal::t_wait_through_busy, true)) {}

Engine::WaitThroughBusyScope::~WaitThroughBusyScope() { engine_internal::t_wait_through_busy = previous_; }

// M6b D6: BeginLoad raises loading_ under the exclusive gate, so a call that holds the gate sees it.
bool Engine::LoadingRefuses(bool read) const {
  return loading_.load(std::memory_order_acquire) && !(read && engine_internal::t_loading_read);
}

bool Engine::BusyGuarded() const { return busy_ && !engine_internal::t_wait_through_busy; }

bool Engine::AcquireShared() {
  if (!BusyGuarded()) {
    gate_.lock_shared();
    return true;
  }
  return gate_.lock_shared_unless(busy_);
}

bool Engine::AcquireExclusive() {
  if (!BusyGuarded()) {
    gate_.lock();
    return true;
  }
  return gate_.lock_unless(busy_);
}

int64_t Engine::NowMs() const {
  if (const Batch* b = ActiveBatch()) return b->now_ms();
  return clock_.NowMs();
}

Status Engine::Read(const ReadFn& fn) {
  if (Batch* b = ActiveBatch()) return b->RunRead(fn);
  [[maybe_unused]] const ReentryCheck reentry;
  if (!AcquireShared()) return BusyStatus();
  std::shared_lock gate(gate_, std::adopt_lock);
  if (LoadingRefuses(/*read=*/true)) return Status::UserError(std::string(kErrLoading));
  auto view = storage_.BeginRead();
  if (!view.ok()) return view.status();
  return fn(**view, clock_.NowMs());
}

Status Engine::Snapshot(const ReadFn& fn) {
  if (Batch* b = ActiveBatch()) return b->RunRead(fn);  // a batch reads its own transaction
  [[maybe_unused]] const ReentryCheck reentry;
  if (!AcquireShared()) return BusyStatus();
  std::shared_lock gate(gate_, std::adopt_lock);
  if (LoadingRefuses(/*read=*/true)) return Status::UserError(std::string(kErrLoading));
  auto view = storage_.BeginSnapshot();
  if (!view.ok()) return view.status();
  return fn(**view, clock_.NowMs());
}

Status Engine::Write(uint8_t db, const std::vector<std::string_view>& keys, const WriteFn& fn,
                     std::vector<std::string>* also_modified) {
  if (Batch* b = ActiveBatch()) return b->RunWrite(fn, WriteScope{db, &keys, also_modified, nullptr});
  [[maybe_unused]] const ReentryCheck reentry;
  if (!AcquireShared()) return BusyStatus();
  std::shared_lock gate(gate_, std::adopt_lock);
  if (LoadingRefuses(/*read=*/false)) return Status::UserError(std::string(kErrLoading));
  if (fence_.AnyKey(keys)) return Status::UserError(std::string(kErrTryAgain));  // M7b D4, under the gate
  const LockManager::Guard guard = locks_.Lock(db, keys);
  return RunTxn(fn, WriteScope{db, &keys, also_modified, nullptr});
}

Status Engine::WriteExclusive(const WriteFn& fn, const std::vector<uint8_t>& flushed_dbs) {
  if (Batch* b = ActiveBatch()) return b->RunWrite(fn, WriteScope{0, nullptr, nullptr, &flushed_dbs});
  [[maybe_unused]] const ReentryCheck reentry;
  if (!AcquireExclusive()) return BusyStatus();
  std::unique_lock gate(gate_, std::adopt_lock);
  if (LoadingRefuses(/*read=*/false)) return Status::UserError(std::string(kErrLoading));
  return RunTxn(fn, WriteScope{0, nullptr, nullptr, &flushed_dbs});
}

Status Engine::CollectFlushed(ReadView& view, const WriteScope& scope, std::vector<DbKey>* out) {
  out->clear();
  if (scope.flushed_dbs == nullptr || observer_ == nullptr || observer_->Idle()) return Status::Ok();
  for (uint8_t db : *scope.flushed_dbs) {
    for (std::string& k : observer_->ExistingWatched(db, view)) out->push_back({db, std::move(k)});
  }
  return Status::Ok();
}

void Engine::Report(const std::vector<DbKey>& keys) {
  if (observer_ == nullptr || keys.empty() || observer_->Idle()) return;
  Result<std::unique_ptr<ReadView>> view = Status::Internal("committed view not opened");
  bool opened = false;
  const CommittedViewFn committed = [&]() -> ReadView* {  // opened only if the observer must probe a key
    if (!opened) {
      opened = true;
      view = storage_.BeginRead();
      if (!view.ok()) spdlog::warn("watch report without a committed view: {}", view.status().ToString());
    }
    return view.ok() ? view->get() : nullptr;
  };
  observer_->OnModified(keys, committed);
}

Status Engine::RunTxn(const WriteFn& fn, const WriteScope& scope) {
  Status st;
  for (int attempt = 1; attempt <= kMaxCommitAttempts; ++attempt) {
    auto txn = storage_.BeginWrite();
    if (!txn.ok()) return txn.status();
    RecordingTxn recorder(**txn);
    recorder.CaptureOps(repl_log_ != nullptr);  // before the first write
    if (scope.also_modified != nullptr) scope.also_modified->clear();
    std::vector<DbKey> flushed;
    if (st = CollectFlushed(recorder, scope, &flushed); !st.ok()) return st;
    st = fn(recorder, clock_.NowMs());  // read under the locks, fresh on every attempt
    if (!st.ok()) return st;            // destroying the unfinished txn rolls it back
    // The ReplLog writes replog[seq] on the inner txn: the recorder refuses the replog CF.
    st = repl_log_ != nullptr ? repl_log_->Commit(**txn, recorder.ops()) : recorder.Commit();
    if (st.ok()) {
      if (listener_ != nullptr && !recorder.deletes().empty()) listener_->OnCommitted(recorder.deletes());
      if (Observing()) {
        Report(ModifiedBy(recorder.writes() > 0, recorder.meta_keys(), scope, std::move(flushed)));
      }
    }
    if (st.code() != StatusCode::kConflict) return st;
    // Key locks make conflicts impossible (invariant I1), so one indicates a bug.
    spdlog::warn("commit conflict, attempt {}/{}: {}", attempt, kMaxCommitAttempts, st.message());
  }
  return st;
}

Status Engine::ApplyReplicated(uint64_t seq, std::string_view payload) {
  if (repl_log_ == nullptr) return Status::Internal("apply without a replication log");
  if (!repl_log_->fenced()) return Status::Internal("apply while local writes are not fenced");
  // G1: never onto a partial keyspace (CF-M6b-4 enforced here, not by caller discipline). ReplLog
  // re-checks the marker under its mutex.
  if (loading() || repl_log_->load_incomplete()) return Status::Internal("apply during a full-sync load");
  if (ActiveBatch() != nullptr) return Status::Internal("apply inside a batch");
  auto batch = encoding::DecodeReplogBatch(payload);
  if (!batch.ok()) return batch.status();
  const std::vector<uint8_t> flushed_dbs = engine_internal::FlushedDbs(batch->ops);
  const WriteScope scope{0, nullptr, nullptr, &flushed_dbs};
  const WaitThroughBusyScope wait_for_scripts;
  [[maybe_unused]] const ReentryCheck reentry;
  if (!AcquireShared()) return BusyStatus();  // unreachable inside the scope; kept for symmetry
  std::shared_lock gate(gate_, std::adopt_lock);
  auto txn = storage_.BeginWrite();
  if (!txn.ok()) return txn.status();
  RecordingTxn recorder(**txn);  // capture off: the master's payload is stored verbatim
  std::vector<DbKey> flushed;
  if (Status st = CollectFlushed(recorder, scope, &flushed); !st.ok()) return st;
  // A failure below leaves the txn uncommitted; destroying it rolls it back.
  if (Status st = ApplyReplOps(recorder, batch->ops); !st.ok()) return st;
  if (Status st = repl_log_->CommitReplicated(**txn, seq, payload); !st.ok()) return st;
  if (listener_ != nullptr && !recorder.deletes().empty()) listener_->OnCommitted(recorder.deletes());
  if (Observing()) Report(ModifiedBy(recorder.writes() > 0, recorder.meta_keys(), scope, std::move(flushed)));
  return Status::Ok();
}

Result<uint64_t> Engine::BatchMark() {
  Batch* b = ActiveBatch();
  if (b == nullptr) return Status::Internal("no active batch");
  return b->Mark();
}

Status Engine::BatchRollbackTo(uint64_t mark) {
  Batch* b = ActiveBatch();
  if (b == nullptr) return Status::Internal("no active batch");
  return b->RollbackTo(mark);
}

Status Engine::BatchReleaseMark(uint64_t mark) {
  Batch* b = ActiveBatch();
  if (b == nullptr) return Status::Internal("no active batch");
  return b->ReleaseMark(mark);
}

Result<std::unique_ptr<Engine::Batch>> Engine::BeginBatch() {
  if (engine_internal::t_batch != nullptr) return Status::Internal("nested batch");
  [[maybe_unused]] const ReentryCheck reentry;
  if (!AcquireExclusive()) return BusyStatus();
  std::unique_lock gate(gate_, std::adopt_lock);
  auto txn = storage_.BeginWrite();
  if (!txn.ok()) return txn.status();
  std::unique_ptr<Batch> batch(new Batch(*this, std::move(gate), std::move(txn).value(), clock_.NowMs()));
  engine_internal::t_batch = batch.get();
  return batch;
}

}  // namespace tideskv
