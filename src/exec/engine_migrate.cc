// The migration entries of the Engine (M7b D4): the slot write fence and the internal writers.
#include <cassert>
#include <cstdint>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "exec/engine.h"
#include "exec/engine_internal.h"
#include "exec/recording_txn.h"
#include "types/errors.h"

namespace tideskv {

uint64_t Engine::ChangeFence(const std::function<void(SlotBits&)>& edit) {
  assert(ActiveBatch() == nullptr && "a fence change inside a batch deadlocks on the gate");
  [[maybe_unused]] const engine_internal::ReentryCheck reentry;
  std::lock_guard barrier(gate_);  // every write holding the gate finishes first (D4), busy or not
  edit(fence_requested_);
  ApplyFence();
  return repl_log_ != nullptr ? repl_log_->last() : 0;  // inside the barrier: the end of the final delta
}

void Engine::ApplyFence() {
  if (fence_holds_ == 0) {
    fence_.Assign(fence_requested_);
    return;
  }
  SlotBits all;
  all.set();
  fence_.Assign(all);
}

void Engine::HoldAllFenced() {
  ChangeFence([this](SlotBits&) { ++fence_holds_; });
}

void Engine::ReleaseAllFenced() {
  ChangeFence([this](SlotBits&) {
    assert(fence_holds_ > 0 && "a release without a hold");
    --fence_holds_;
  });
}

uint64_t Engine::FenceSlots(uint16_t first, uint16_t last) {
  return ChangeFence([&](SlotBits& b) {
    for (uint32_t s = first; s <= last && s < kSlotCount; ++s) b.set(s);
  });
}

uint64_t Engine::UnfenceSlots(uint16_t first, uint16_t last) {
  return ChangeFence([&](SlotBits& b) {
    for (uint32_t s = first; s <= last && s < kSlotCount; ++s) b.reset(s);
  });
}

uint64_t Engine::AssignFence(const SlotBits& bits) {
  return ChangeFence([&](SlotBits& b) { b = bits; });
}

Status Engine::ApplyImport(const std::vector<encoding::ReplOp>& ops) { return CommitInternal(ops); }

Status Engine::WriteInternal(const std::vector<encoding::ReplOp>& ops) { return CommitInternal(ops); }

// The shape of RunTxn without key stripes or retries: the ops are physical, so a conflict (impossible
// without another writer of the range, D22) is returned to the caller.
Status Engine::CommitInternal(const std::vector<encoding::ReplOp>& ops) {
  if (ops.empty()) return Status::Ok();
  if (ActiveBatch() != nullptr) return Status::Internal("a migration write inside a batch");
  [[maybe_unused]] const engine_internal::ReentryCheck reentry;
  if (!AcquireShared()) return BusyStatus();
  std::shared_lock gate(gate_, std::adopt_lock);
  if (LoadingRefuses(/*read=*/false)) return Status::UserError(std::string(kErrLoading));
  auto txn = storage_.BeginWrite();
  if (!txn.ok()) return txn.status();
  RecordingTxn recorder(**txn);
  recorder.CaptureOps(repl_log_ != nullptr);                         // before the first write
  if (Status st = ApplyReplOps(recorder, ops); !st.ok()) return st;  // destroying the txn rolls it back
  Status st = repl_log_ != nullptr ? repl_log_->Commit(**txn, recorder.ops()) : recorder.Commit();
  if (!st.ok()) return st;
  if (listener_ != nullptr && !recorder.deletes().empty()) listener_->OnCommitted(recorder.deletes());
  if (Observing()) {
    Report(engine_internal::ModifiedBy(recorder.writes() > 0, recorder.meta_keys(),
                                       engine_internal::WriteScope{}, {}));
  }
  return Status::Ok();
}

}  // namespace tideskv
