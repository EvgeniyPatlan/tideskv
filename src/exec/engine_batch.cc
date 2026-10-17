#include <spdlog/spdlog.h>

#include <algorithm>
#include <span>
#include <string>

#include "exec/engine.h"
#include "exec/engine_internal.h"
#include "types/errors.h"

namespace tideskv {

using engine_internal::ModifiedBy;
using engine_internal::ReentryCheck;
using engine_internal::WriteScope;

Engine::Batch::Batch(Engine& engine, std::unique_lock<Gate> gate, std::unique_ptr<WriteTxn> txn,
                     int64_t now_ms)
    : engine_(engine), gate_(std::move(gate)), txn_(std::move(txn)), recorder_(*txn_), now_ms_(now_ms) {
  recorder_.CaptureOps(engine_.repl_log_ != nullptr);  // before the batch's first write
}

Engine::Batch::~Batch() {
  if (done_) return;
  Unroute();
  if (Status st = recorder_.Rollback(); !st.ok()) spdlog::warn("batch rollback: {}", st.ToString());
}

void Engine::Batch::Unroute() {
  if (engine_internal::t_batch == this) engine_internal::t_batch = nullptr;
}

Status Engine::Batch::RunRead(const ReadFn& fn) {
  [[maybe_unused]] const ReentryCheck reentry;
  // PF7: the batch itself runs while loading (an EXEC touching no data); its keyspace access does not.
  if (engine_.LoadingRefuses(/*read=*/true)) return Status::UserError(std::string(kErrLoading));
  return fn(recorder_, now_ms_);
}

Status Engine::Batch::RunWrite(const WriteFn& fn, const WriteScope& scope) {
  [[maybe_unused]] const ReentryCheck reentry;
  if (engine_.LoadingRefuses(/*read=*/false)) return Status::UserError(std::string(kErrLoading));
  // M7b D4 backstop for undeclared keys of a script; EXEC and EVAL refuse a fenced call as a whole (Task 5).
  if (scope.keys != nullptr && engine_.fence_.AnyKey(*scope.keys)) {
    return Status::UserError(std::string(kErrTryAgain));
  }
  if (!broken_.ok()) return broken_;
  const std::string savepoint = "exec" + std::to_string(next_savepoint_++);
  if (Status st = recorder_.Savepoint(savepoint); !st.ok()) return st;
  const size_t writes_before = recorder_.writes();
  const size_t keys_before = recorder_.meta_keys().size();
  if (scope.also_modified != nullptr) scope.also_modified->clear();
  std::vector<DbKey> flushed;
  Status st = engine_.CollectFlushed(recorder_, scope, &flushed);
  if (st.ok()) {
    try {
      st = fn(recorder_, now_ms_);
    } catch (...) {
      UndoWrite(savepoint);
      throw;
    }
  }
  if (!st.ok()) {
    UndoWrite(savepoint);
    return st;
  }
  Release(savepoint);  // keeps TidesDB's savepoint list at (live marks + 1): see storage.h
  const std::span<const DbKey> meta(recorder_.meta_keys());
  std::vector<DbKey> keys =
      ModifiedBy(recorder_.writes() > writes_before, meta.subspan(keys_before), scope, std::move(flushed));
  modified_.insert(modified_.end(), keys.begin(), keys.end());
  return Status::Ok();
}

// Known TidesDB cost: a rollback that discards ops rebuilds the write set's key index
// (tidesdb_writeset_truncate), O(batch size) per rollback. Only a command that wrote and then failed pays
// it; one that failed before writing has nothing to discard.
void Engine::Batch::UndoWrite(const std::string& savepoint) {
  if (Status rb = recorder_.RollbackToSavepoint(savepoint); !rb.ok()) {
    broken_ = rb;
    return;
  }
  Release(savepoint);  // the rollback keeps the target savepoint
}

void Engine::Batch::Release(const std::string& savepoint) {
  if (Status st = recorder_.ReleaseSavepoint(savepoint); !st.ok())
    spdlog::warn("batch savepoint release: {}", st.ToString());
}

Result<uint64_t> Engine::Batch::Mark() {
  if (!broken_.ok()) return broken_;
  const uint64_t savepoint = next_savepoint_++;
  if (Status st = recorder_.Savepoint("mark" + std::to_string(savepoint)); !st.ok()) return st;
  marks_.push_back({savepoint, modified_.size()});
  return marks_.size() - 1;
}

Status Engine::Batch::RollbackTo(uint64_t mark) {
  if (mark >= marks_.size()) return Status::Internal("unknown batch mark");
  if (!broken_.ok()) return broken_;
  if (Status st = recorder_.RollbackToSavepoint("mark" + std::to_string(marks_[mark].savepoint)); !st.ok()) {
    broken_ = st;
    return st;
  }
  modified_.resize(marks_[mark].modified);
  marks_.resize(mark + 1);  // later marks were undone with their writes: never roll back to them
  return Status::Ok();
}

Status Engine::Batch::ReleaseMark(uint64_t mark) {
  if (mark + 1 != marks_.size()) return Status::Internal("not the newest batch mark");
  if (!broken_.ok()) return broken_;
  const Status st = recorder_.ReleaseSavepoint("mark" + std::to_string(marks_[mark].savepoint));
  if (!st.ok()) return st;
  marks_.pop_back();  // its writes and modified keys now belong to the mark before it, if any
  return st;
}

Status Engine::Batch::CommitTxn() {
  if (engine_.repl_log_ == nullptr) return recorder_.Commit();
  return engine_.repl_log_->Commit(*txn_, recorder_.ops());  // the recorder refuses the replog CF
}

Status Engine::Batch::Commit() {
  if (done_) return Status::Internal("batch already finished");
  done_ = true;
  Unroute();
  if (!broken_.ok()) {
    if (Status rb = recorder_.Rollback(); !rb.ok()) spdlog::warn("batch rollback: {}", rb.ToString());
    return broken_;
  }
  Status st = CommitTxn();  // no retry: the queued commands' replies are already written
  if (!st.ok()) {
    if (Status rb = recorder_.Rollback(); !rb.ok()) spdlog::warn("batch rollback: {}", rb.ToString());
    return st;
  }
  if (engine_.listener_ != nullptr && !recorder_.deletes().empty())
    engine_.listener_->OnCommitted(recorder_.deletes());
  if (engine_.Observing()) {
    std::sort(modified_.begin(), modified_.end());
    modified_.erase(std::unique(modified_.begin(), modified_.end()), modified_.end());
    engine_.Report(modified_);
  }
  return st;
}

}  // namespace tideskv
