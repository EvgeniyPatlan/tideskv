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
  if (layout) return BeginLoadAdopting(extra, *layout);
  {
    [[maybe_unused]] const engine_internal::ReentryCheck reentry;
    std::lock_guard barrier(gate_);  // every command holding the gate finishes first (D6), busy or not
    if (loading()) return Status::Internal("a full-sync load is already running");
    loading_.store(true, std::memory_order_release);
  }
  const Status st = RunLoadTxn(
      WithWipe(extra), [this](WriteTxn& t) { return repl_log_->CommitLoadStart(t); },
      [this] {
        if (observer_ != nullptr) observer_->OnDatasetReplaced();
      });
  if (!st.ok()) loading_.store(false, std::memory_order_release);
  return st;
}

// A LoadingReadScope reader (DEBUG DIGEST, WATCH) runs while loading, so the codec must never meet data of
// the other layout: the wipe commits and the layout flips both inside the exclusive barrier, and a reader
// sees either the old data with the old layout or the empty keyspace with the new one.
Status Engine::BeginLoadAdopting(const std::vector<encoding::ReplOp>& extra, encoding::KeyLayout layout) {
  Status st;
  {
    [[maybe_unused]] const engine_internal::ReentryCheck reentry;
    std::lock_guard barrier(gate_);  // every command holding the gate finishes first (D6), busy or not
    if (loading()) return Status::Internal("a full-sync load is already running");
    loading_.store(true, std::memory_order_release);
    auto txn = storage_.BeginWrite();
    st = txn.status();
    if (st.ok()) st = ApplyReplOps(**txn, WithWipe(extra));  // destroying the txn rolls it back
    if (st.ok()) st = repl_log_->CommitLoadStart(**txn);
    if (st.ok()) {
      encoding::AdoptKeyLayout(layout);
    } else {
      loading_.store(false, std::memory_order_release);  // nothing was wiped: the old layout stays
    }
  }
  if (st.ok() && observer_ != nullptr) observer_->OnDatasetReplaced();
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
