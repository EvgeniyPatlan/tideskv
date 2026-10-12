#include "exec/engine.h"

#include <spdlog/spdlog.h>

#include <mutex>
#include <shared_mutex>

namespace tideskv {

Status Engine::Read(const ReadFn& fn) {
  std::shared_lock gate(gate_);
  auto view = storage_.BeginRead();
  if (!view.ok()) return view.status();
  return fn(**view);
}

Status Engine::Snapshot(const ReadFn& fn) {
  std::shared_lock gate(gate_);
  auto view = storage_.BeginSnapshot();
  if (!view.ok()) return view.status();
  return fn(**view);
}

Status Engine::Write(uint8_t db, const std::vector<std::string_view>& keys, const WriteFn& fn) {
  std::shared_lock gate(gate_);
  const LockManager::Guard guard = locks_.Lock(db, keys);
  return RunTxn(fn);
}

Status Engine::WriteExclusive(const WriteFn& fn) {
  std::unique_lock gate(gate_);
  return RunTxn(fn);
}

Status Engine::RunTxn(const WriteFn& fn) {
  Status st;
  for (int attempt = 1; attempt <= kMaxCommitAttempts; ++attempt) {
    auto txn = storage_.BeginWrite();
    if (!txn.ok()) return txn.status();
    st = fn(**txn);
    if (!st.ok()) return st;  // destroying the unfinished txn rolls it back
    st = (*txn)->Commit();
    if (st.code() != StatusCode::kConflict) return st;
    // Key locks make conflicts impossible (invariant I1), so one indicates a bug.
    spdlog::warn("commit conflict, attempt {}/{}: {}", attempt, kMaxCommitAttempts, st.message());
  }
  return st;
}

}  // namespace tideskv
