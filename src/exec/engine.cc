#include "exec/engine.h"

#include <spdlog/spdlog.h>

#include <mutex>
#include <shared_mutex>

#include "exec/engine_internal.h"
#include "exec/recording_txn.h"

namespace tideskv {

using engine_internal::ModifiedBy;
using engine_internal::ReentryCheck;
using engine_internal::WriteScope;

Engine::Batch* Engine::ActiveBatch() const {
  return engine_internal::t_batch != nullptr && &engine_internal::t_batch->engine_ == this ? engine_internal::t_batch
                                                                                          : nullptr;
}

int64_t Engine::NowMs() const {
  if (const Batch* b = ActiveBatch()) return b->now_ms();
  return clock_.NowMs();
}

Status Engine::Read(const ReadFn& fn) {
  if (Batch* b = ActiveBatch()) return b->RunRead(fn);
  [[maybe_unused]] const ReentryCheck reentry;
  std::shared_lock gate(gate_);
  auto view = storage_.BeginRead();
  if (!view.ok()) return view.status();
  return fn(**view, clock_.NowMs());
}

Status Engine::Snapshot(const ReadFn& fn) {
  if (Batch* b = ActiveBatch()) return b->RunRead(fn);  // a batch reads its own transaction
  [[maybe_unused]] const ReentryCheck reentry;
  std::shared_lock gate(gate_);
  auto view = storage_.BeginSnapshot();
  if (!view.ok()) return view.status();
  return fn(**view, clock_.NowMs());
}

Status Engine::Write(uint8_t db, const std::vector<std::string_view>& keys, const WriteFn& fn,
                     std::vector<std::string>* also_modified) {
  if (Batch* b = ActiveBatch()) return b->RunWrite(fn, WriteScope{db, &keys, also_modified, nullptr});
  [[maybe_unused]] const ReentryCheck reentry;
  std::shared_lock gate(gate_);
  const LockManager::Guard guard = locks_.Lock(db, keys);
  return RunTxn(fn, WriteScope{db, &keys, also_modified, nullptr});
}

Status Engine::WriteExclusive(const WriteFn& fn, const std::vector<uint8_t>& flushed_dbs) {
  if (Batch* b = ActiveBatch()) return b->RunWrite(fn, WriteScope{0, nullptr, nullptr, &flushed_dbs});
  [[maybe_unused]] const ReentryCheck reentry;
  std::unique_lock gate(gate_);
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
  auto view = storage_.BeginRead();
  if (!view.ok()) spdlog::warn("watch report without a committed view: {}", view.status().ToString());
  observer_->OnModified(keys, view.ok() ? view->get() : nullptr);
}

Status Engine::RunTxn(const WriteFn& fn, const WriteScope& scope) {
  Status st;
  for (int attempt = 1; attempt <= kMaxCommitAttempts; ++attempt) {
    auto txn = storage_.BeginWrite();
    if (!txn.ok()) return txn.status();
    RecordingTxn recorder(**txn);
    if (scope.also_modified != nullptr) scope.also_modified->clear();
    std::vector<DbKey> flushed;
    if (st = CollectFlushed(recorder, scope, &flushed); !st.ok()) return st;
    st = fn(recorder, clock_.NowMs());  // read under the locks, fresh on every attempt
    if (!st.ok()) return st;            // destroying the unfinished txn rolls it back
    st = recorder.Commit();
    if (st.ok()) {
      if (listener_ != nullptr && !recorder.deletes().empty()) listener_->OnCommitted(recorder.deletes());
      Report(ModifiedBy(recorder.writes() > 0, recorder.meta_keys(), scope, std::move(flushed)));
    }
    if (st.code() != StatusCode::kConflict) return st;
    // Key locks make conflicts impossible (invariant I1), so one indicates a bug.
    spdlog::warn("commit conflict, attempt {}/{}: {}", attempt, kMaxCommitAttempts, st.message());
  }
  return st;
}

Result<std::unique_ptr<Engine::Batch>> Engine::BeginBatch() {
  if (engine_internal::t_batch != nullptr) return Status::Internal("nested batch");
  [[maybe_unused]] const ReentryCheck reentry;
  std::unique_lock gate(gate_);
  auto txn = storage_.BeginWrite();
  if (!txn.ok()) return txn.status();
  std::unique_ptr<Batch> batch(new Batch(*this, std::move(gate), std::move(txn).value(), clock_.NowMs()));
  engine_internal::t_batch = batch.get();
  return batch;
}

}  // namespace tideskv
