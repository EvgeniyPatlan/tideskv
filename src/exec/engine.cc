#include "exec/engine.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <mutex>
#include <shared_mutex>

namespace tideskv {
namespace {

#ifndef NDEBUG
thread_local bool t_in_engine = false;

// A nested Engine call from inside a fn would self-deadlock on the non-reentrant Gate.
class ReentryCheck {
 public:
  ReentryCheck() {
    assert(!t_in_engine && "an Engine fn called back into the Engine");
    t_in_engine = true;
  }
  ~ReentryCheck() { t_in_engine = false; }
  ReentryCheck(const ReentryCheck&) = delete;
  ReentryCheck& operator=(const ReentryCheck&) = delete;
};
#else
struct ReentryCheck {};
#endif

}  // namespace

Status Engine::Read(const ReadFn& fn) {
  [[maybe_unused]] const ReentryCheck reentry;
  std::shared_lock gate(gate_);
  auto view = storage_.BeginRead();
  if (!view.ok()) return view.status();
  return fn(**view, clock_.NowMs());
}

Status Engine::Snapshot(const ReadFn& fn) {
  [[maybe_unused]] const ReentryCheck reentry;
  std::shared_lock gate(gate_);
  auto view = storage_.BeginSnapshot();
  if (!view.ok()) return view.status();
  return fn(**view, clock_.NowMs());
}

Status Engine::Write(uint8_t db, const std::vector<std::string_view>& keys, const WriteFn& fn) {
  [[maybe_unused]] const ReentryCheck reentry;
  std::shared_lock gate(gate_);
  const LockManager::Guard guard = locks_.Lock(db, keys);
  return RunTxn(fn);
}

Status Engine::WriteExclusive(const WriteFn& fn) {
  [[maybe_unused]] const ReentryCheck reentry;
  std::unique_lock gate(gate_);
  return RunTxn(fn);
}

Status Engine::RunTxn(const WriteFn& fn) {
  Status st;
  for (int attempt = 1; attempt <= kMaxCommitAttempts; ++attempt) {
    auto txn = storage_.BeginWrite();
    if (!txn.ok()) return txn.status();
    st = fn(**txn, clock_.NowMs());  // read under the locks, fresh on every attempt
    if (!st.ok()) return st;         // destroying the unfinished txn rolls it back
    st = (*txn)->Commit();
    if (st.code() != StatusCode::kConflict) return st;
    // Key locks make conflicts impossible (invariant I1), so one indicates a bug.
    spdlog::warn("commit conflict, attempt {}/{}: {}", attempt, kMaxCommitAttempts, st.message());
  }
  return st;
}

}  // namespace tideskv
