#include <spdlog/spdlog.h>

#include <algorithm>
#include <span>
#include <string>

#include "exec/engine.h"
#include "exec/engine_internal.h"

namespace tideskv {

using engine_internal::ModifiedBy;
using engine_internal::ReentryCheck;
using engine_internal::WriteScope;

Engine::Batch::Batch(Engine& engine, std::unique_lock<Gate> gate, std::unique_ptr<WriteTxn> txn,
                     int64_t now_ms)
    : engine_(engine), gate_(std::move(gate)), txn_(std::move(txn)), recorder_(*txn_), now_ms_(now_ms) {}

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
  return fn(recorder_, now_ms_);
}

Status Engine::Batch::RunWrite(const WriteFn& fn, const WriteScope& scope) {
  [[maybe_unused]] const ReentryCheck reentry;
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
      if (Status rb = recorder_.RollbackToSavepoint(savepoint); !rb.ok()) broken_ = rb;
      throw;
    }
  }
  if (!st.ok()) {
    if (Status rb = recorder_.RollbackToSavepoint(savepoint); !rb.ok()) broken_ = rb;
    return st;
  }
  const std::span<const DbKey> meta(recorder_.meta_keys());
  std::vector<DbKey> keys =
      ModifiedBy(recorder_.writes() > writes_before, meta.subspan(keys_before), scope, std::move(flushed));
  modified_.insert(modified_.end(), keys.begin(), keys.end());
  return Status::Ok();
}

Status Engine::Batch::Commit() {
  if (done_) return Status::Internal("batch already finished");
  done_ = true;
  Unroute();
  if (!broken_.ok()) {
    if (Status rb = recorder_.Rollback(); !rb.ok()) spdlog::warn("batch rollback: {}", rb.ToString());
    return broken_;
  }
  Status st = recorder_.Commit();  // no retry: the queued commands' replies are already written
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
