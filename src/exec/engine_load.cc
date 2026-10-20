// A replica's full-sync load (M6b D5, D6): the Engine's loading state and its load transactions.
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "encoding/key_layout.h"
#include "exec/engine.h"
#include "exec/engine_internal.h"

namespace tideskv {
namespace {

constexpr std::string_view kWholeCfLo("\x00", 1);  // with an empty hi: every key of the CF
constexpr ColumnFamily kKeyspaceCfs[] = {ColumnFamily::kMeta, ColumnFamily::kSubkey, ColumnFamily::kZScore,
                                         ColumnFamily::kExpire};

// The keyspace wipe, then `extra`.
std::vector<encoding::ReplOp> WithWipe(const std::vector<encoding::ReplOp>& extra) {
  std::vector<encoding::ReplOp> ops;
  ops.reserve(std::size(kKeyspaceCfs) + extra.size());
  for (ColumnFamily cf : kKeyspaceCfs) {
    ops.push_back({encoding::ReplOpType::kDeleteRange, cf, std::string(kWholeCfLo), ""});
  }
  ops.insert(ops.end(), extra.begin(), extra.end());
  return ops;
}

// What compaction should reclaim once a load ends: the keyspace wipes and the replog wipe of
// ReplLog::CommitLoadStart.
std::vector<RangeDelete> WipeDeletes() {
  std::vector<RangeDelete> out;
  for (ColumnFamily cf : kKeyspaceCfs) out.push_back({cf, std::string(kWholeCfLo), "", false});
  out.push_back({ColumnFamily::kReplog, std::string(kWholeCfLo), "", false});
  return out;
}

}  // namespace

Engine::LoadingReadScope::LoadingReadScope()
    : previous_(std::exchange(engine_internal::t_loading_read, true)) {}

Engine::LoadingReadScope::~LoadingReadScope() { engine_internal::t_loading_read = previous_; }

// Under the shared gate, as ApplyReplicated: a replica has no other committer (its ReplLog is fenced, or,
// at startup, nothing else runs yet), and a batch or script holds the gate exclusively. Waits through a
// busy script, as Redis loads after one.
Status Engine::RunLoadTxn(const std::vector<encoding::ReplOp>& ops,
                          const std::function<Status(WriteTxn&)>& commit,
                          const std::function<void()>& committed) {
  const WaitThroughBusyScope wait_for_scripts;
  [[maybe_unused]] const engine_internal::ReentryCheck reentry;
  if (!AcquireShared()) return BusyStatus();  // unreachable inside the scope; kept for symmetry
  std::shared_lock gate(gate_, std::adopt_lock);
  auto txn = storage_.BeginWrite();
  if (!txn.ok()) return txn.status();
  if (Status st = ApplyReplOps(**txn, ops); !st.ok()) return st;  // destroying the txn rolls it back
  if (Status st = commit(**txn); !st.ok()) return st;
  if (committed) committed();
  return Status::Ok();
}

Status Engine::BeginLoad(const std::vector<encoding::ReplOp>& extra,
                         std::optional<encoding::KeyLayout> layout) {
  if (repl_log_ == nullptr || !repl_log_->fenced())
    return Status::Internal("full-sync load needs a fenced log");
  if (ActiveBatch() != nullptr) return Status::Internal("full-sync load inside a batch");
  const encoding::KeyLayout previous = encoding::CurrentKeyLayout();
  {
    [[maybe_unused]] const engine_internal::ReentryCheck reentry;
    std::lock_guard barrier(gate_);  // every command holding the gate finishes first (D6), busy or not
    if (loading()) return Status::Internal("a full-sync load is already running");
    loading_.store(true, std::memory_order_release);
    if (layout) encoding::AdoptKeyLayout(*layout);  // every command is refused (kErrLoading) from here on
  }
  const Status st = RunLoadTxn(
      WithWipe(extra), [this](WriteTxn& t) { return repl_log_->CommitLoadStart(t); },
      [this] {
        if (observer_ != nullptr) observer_->OnDatasetReplaced();
      });
  if (!st.ok()) {
    if (layout) encoding::AdoptKeyLayout(previous);  // nothing was wiped: the data is still in that layout
    loading_.store(false, std::memory_order_release);
  }
  return st;
}

Status Engine::LoadChunk(const std::vector<encoding::ReplOp>& ops) {
  if (!loading()) return Status::Internal("a full-sync chunk outside a load");
  return RunLoadTxn(ops, [](WriteTxn& t) { return t.Commit(); }, nullptr);
}

// CF-M6b-2: ReplLog::CommitLoadEnd does not check that a load started. loading() alone holds from the
// barrier on, before CommitLoadStart committed; the marker is set only once it did.
Status Engine::EndLoad(uint64_t seq, std::string_view record, const std::vector<encoding::ReplOp>& extra) {
  if (!loading() || !repl_log_->load_incomplete()) return Status::Internal("full-sync end outside a load");
  return RunLoadTxn(
      extra, [&](WriteTxn& t) { return repl_log_->CommitLoadEnd(t, seq, record); }, [this] { FinishLoad(); });
}

Status Engine::AbortLoad() {
  if (repl_log_ == nullptr) return Status::Internal("full-sync abort without a log");
  if (!loading() && !repl_log_->load_incomplete()) return Status::Internal("no full-sync load to discard");
  return RunLoadTxn(
      WithWipe({}), [this](WriteTxn& t) { return repl_log_->CommitLoadAbort(t); }, [this] { FinishLoad(); });
}

void Engine::FinishLoad() {
  if (listener_ != nullptr) listener_->OnCommitted(WipeDeletes());
  if (observer_ != nullptr) observer_->OnDatasetReplaced();
  loading_.store(false, std::memory_order_release);
}

}  // namespace tideskv
