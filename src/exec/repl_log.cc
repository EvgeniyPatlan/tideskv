#include "exec/repl_log.h"

#include <string>
#include <utility>

#include "common/coding.h"
#include "exec/thread_commit_seq.h"
#include "types/errors.h"

namespace tideskv {
namespace {

// sys/repl_first: 8-byte BE seq of the oldest retained replog entry (written by trimming).
constexpr std::string_view kReplFirstKey = "repl_first";
// = repl::kSysReplBaselineUnknown (repl/ is not included here: exec -> repl would be a cycle).
constexpr std::string_view kBaselineUnknownKey = "repl_baseline_unknown";
// With an empty hi (= to the end): every key of a CF.
constexpr std::string_view kWholeCfLo("\x00", 1);

Status ApplyOp(WriteTxn& txn, const encoding::ReplOp& op) {
  switch (op.type) {
    case encoding::ReplOpType::kPut:
      return txn.Put(op.cf, op.key, op.value);
    case encoding::ReplOpType::kDelete:
      return txn.Delete(op.cf, op.key);
    case encoding::ReplOpType::kDeletePrefix:
      return txn.DeletePrefix(op.cf, op.key);
    case encoding::ReplOpType::kDeleteRange:
      return txn.DeleteRange(op.cf, op.key, op.value);
  }
  return Status::InvalidArgument("unknown replication op type");
}

}  // namespace

Status ApplyReplOps(WriteTxn& txn, const std::vector<encoding::ReplOp>& ops) {
  for (const encoding::ReplOp& op : ops) {
    if (Status st = ApplyOp(txn, op); !st.ok()) return st;
  }
  return Status::Ok();
}

Status ReplLog::RecoverLast(ReadView& view, uint64_t* last, uint64_t* cum) {
  *last = 0;
  *cum = 0;
  auto it = view.NewIterator(ColumnFamily::kReplog);
  if (!it.ok()) return it.status();
  if (Status st = (*it)->SeekToLast(); !st.ok()) return st.IsNotFound() ? Status::Ok() : st;
  if (!(*it)->Valid()) return Status::Ok();
  int64_t timestamp_ms = 0;
  if (!encoding::DecodeReplogKey((*it)->key(), last) ||
      !encoding::PeekReplogHeader((*it)->value(), &timestamp_ms, cum)) {
    return Status::Corruption("the last replog entry is malformed");
  }
  return Status::Ok();
}

Status ReplLog::RecoverFirst(ReadView& view, uint64_t last, uint64_t* first) {
  *first = 1;
  auto raw = view.Get(ColumnFamily::kSys, kReplFirstKey);
  if (raw.ok()) {
    std::string_view in = *raw;
    if (in.size() != 8 || !coding::GetFixed64BE(&in, first)) {
      return Status::Corruption("sys/repl_first is not a valid 8-byte seq");
    }
    return Status::Ok();
  }
  if (!raw.status().IsNotFound()) return raw.status();
  if (last == 0) return Status::Ok();
  // Never trimmed (the replog is new in M6a): the oldest entry is the first retained one.
  auto it = view.NewIterator(ColumnFamily::kReplog);
  if (!it.ok()) return it.status();
  if (Status st = (*it)->SeekToFirst(); !st.ok()) return st;
  if (!(*it)->Valid() || !encoding::DecodeReplogKey((*it)->key(), first)) {
    return Status::Corruption("the first replog entry is malformed");
  }
  return Status::Ok();
}

Status ReplLog::RecoverLoadMarker(ReadView& view, bool* incomplete) {
  auto raw = view.Get(ColumnFamily::kSys, kSysReplLoadIncomplete);
  *incomplete = raw.ok();
  if (raw.ok() || raw.status().IsNotFound()) return Status::Ok();
  return raw.status();
}

Status ReplLog::Recover() {
  auto view = storage_.BeginRead();
  if (!view.ok()) return view.status();
  uint64_t last = 0;
  uint64_t cum = 0;
  uint64_t first = 1;
  bool incomplete = false;
  if (Status st = RecoverLast(**view, &last, &cum); !st.ok()) return st;
  if (Status st = RecoverFirst(**view, last, &first); !st.ok()) return st;
  if (Status st = RecoverLoadMarker(**view, &incomplete); !st.ok()) return st;
  // CF3: trimming keeps the newest batch, so first <= last; last + 1 means everything up to last is
  // gone (a full sync's baseline, M6b). Anything else is a damaged sys/repl_first.
  if (first == 0 || first > last + 1) {
    return Status::Corruption("sys/repl_first " + std::to_string(first) + " is outside [1, " +
                              std::to_string(last + 1) + "]");
  }
  std::lock_guard lock(mu_);
  cum_ = cum;
  first_.store(first, std::memory_order_release);
  last_.store(last, std::memory_order_release);
  load_incomplete_.store(incomplete, std::memory_order_release);
  return Status::Ok();
}

Status ReplLog::AppendAndCommit(WriteTxn& txn, uint64_t seq, std::string_view record, uint64_t cum) {
  if (Status st = txn.Put(ColumnFamily::kReplog, encoding::ReplogKey(seq), record); !st.ok()) return st;
  if (Status st = txn.Commit(); !st.ok()) return st;
  cum_ = cum;
  last_.store(seq, std::memory_order_release);
  advanced_.notify_all();
  return Status::Ok();
}

Status ReplLog::Commit(WriteTxn& txn, const std::vector<encoding::ReplOp>& ops) {
  if (ops.empty()) return txn.Commit();
  std::lock_guard lock(mu_);
  // Fenced (replica): refuse without touching txn; the caller's failure path rolls it back as for
  // any failed commit (RunTxn destroys it, Batch::Commit calls Rollback). Checked under mu_ so that
  // FenceLocalWrites(true) is a barrier (CF1).
  if (fenced()) return Status::UserError(std::string(kErrReadOnlyReplica));
  const uint64_t seq = last_.load(std::memory_order_relaxed) + 1;
  const std::string record = encoding::EncodeReplogBatch(ops, clock_.NowMs(), cum_);
  // R12: a batch no replica would accept is refused like a fenced commit: nothing taken, nothing written.
  if (record.size() > max_batch_bytes()) return Status::UserError(std::string(kErrTooLargeToReplicate));
  if (Status st = AppendAndCommit(txn, seq, record, cum_ + record.size()); !st.ok()) return st;
  engine_internal::t_last_commit_seq = seq;  // this thread's write, for WAIT (M6b D9)
  return Status::Ok();
}

void ReplLog::FenceLocalWrites(bool fenced) {
  std::lock_guard lock(mu_);  // waits for an in-flight commit section to finish
  fenced_.store(fenced, std::memory_order_release);
}

Status ReplLog::CommitStandalone(const std::vector<encoding::ReplOp>& ops) {
  // A cheap early refusal; Commit re-checks under mu_.
  if (fenced() && !ops.empty()) return Status::UserError(std::string(kErrReadOnlyReplica));
  auto txn = storage_.BeginWrite();
  if (!txn.ok()) return txn.status();
  if (Status st = ApplyReplOps(**txn, ops); !st.ok()) return st;
  return Commit(**txn, ops);
}

Status ReplLog::CommitReplicated(WriteTxn& txn, uint64_t seq, std::string_view payload) {
  int64_t timestamp_ms = 0;
  uint64_t cum = 0;
  if (!encoding::PeekReplogHeader(payload, &timestamp_ms, &cum)) {
    return Status::Corruption("replicated batch has no valid header");
  }
  std::lock_guard lock(mu_);
  // G1: a partial load's keyspace is no base for the stream (the abort, or a full load, comes first).
  if (load_incomplete()) return Status::Internal("replicated batch during an unfinished full-sync load");
  const uint64_t expected = last_.load(std::memory_order_relaxed) + 1;
  if (seq != expected) {
    return Status::Internal("replicated seq " + std::to_string(seq) + " is not the next seq " +
                            std::to_string(expected));
  }
  // CF2: the master's stream must continue ours byte for byte. No exception for an empty replica
  // (last() == 0): only seq 1 is then accepted, and seq 1's cum_bytes is always its own size (every
  // log starts at 0), so an empty replica's baseline of 0 is exact. A full sync that installs another
  // baseline sets last/cum itself rather than trusting a batch's header.
  if (cum != cum_ + payload.size()) {
    return Status::Corruption("replicated batch " + std::to_string(seq) + " has cum_bytes " +
                              std::to_string(cum) + ", expected " + std::to_string(cum_ + payload.size()));
  }
  return AppendAndCommit(txn, seq, payload, cum);
}

uint64_t ReplLog::cum_bytes() const {
  std::lock_guard lock(mu_);
  return cum_;
}

uint64_t ReplLog::WaitBeyond(uint64_t seq, std::chrono::milliseconds timeout) const {
  std::unique_lock lock(mu_);
  advanced_.wait_for(lock, timeout, [&] { return last_.load(std::memory_order_relaxed) > seq; });
  return last_.load(std::memory_order_relaxed);
}

void ReplLog::SetFirstRetained(uint64_t first) { first_.store(first, std::memory_order_release); }

// One txn: range-delete replog[first .. up_to] and persist repl_first = up_to + 1 (D18). Begun and
// committed under mu_: no commit section runs meanwhile, and the keys it writes are disjoint from any
// commit section's (replog[last()+1] and keyspace CFs), so it cannot hit or cause a kConflict.
Status ReplLog::TrimLocked(uint64_t up_to) {
  const uint64_t last = last_.load(std::memory_order_relaxed);
  if (up_to >= last) {
    return Status::InvalidArgument("cannot trim the replication log to " + std::to_string(up_to) +
                                   ": the newest batch " + std::to_string(last) + " is always kept");
  }
  const uint64_t first = first_.load(std::memory_order_relaxed);
  if (up_to < first) return Status::Ok();
  auto txn = storage_.BeginWrite();
  if (!txn.ok()) return txn.status();
  std::string next;
  coding::PutFixed64BE(&next, up_to + 1);
  if (Status st = (*txn)->DeleteRange(ColumnFamily::kReplog, encoding::ReplogKey(first),
                                      encoding::ReplogKey(up_to + 1));
      !st.ok()) {
    return st;
  }
  if (Status st = (*txn)->Put(ColumnFamily::kSys, kReplFirstKey, next); !st.ok()) return st;
  if (Status st = (*txn)->Commit(); !st.ok()) return st;
  first_.store(up_to + 1, std::memory_order_release);
  return Status::Ok();
}

Status ReplLog::Trim(uint64_t up_to) {
  std::lock_guard lock(mu_);
  return TrimLocked(up_to);
}

Status ReplLog::TrimUnlessFenced(uint64_t up_to, bool* trimmed) {
  *trimmed = false;
  std::lock_guard lock(mu_);
  if (fenced()) return Status::Ok();
  if (Status st = TrimLocked(up_to); !st.ok()) return st;
  *trimmed = true;
  return Status::Ok();
}

Result<SyncSnapshot> ReplLog::SnapshotAtLast() {
  std::lock_guard lock(mu_);
  auto view = storage_.BeginSnapshot();
  if (!view.ok()) return view.status();
  return SyncSnapshot{std::move(view).value(), last_.load(std::memory_order_relaxed)};
}

void ReplLog::SetBaselineLocked(uint64_t last, uint64_t cum, uint64_t first) {
  cum_ = cum;
  first_.store(first, std::memory_order_release);
  last_.store(last, std::memory_order_release);
  advanced_.notify_all();
}

Status ReplLog::CommitLoadStart(WriteTxn& txn) {
  std::lock_guard lock(mu_);
  if (!fenced()) return Status::Internal("full-sync load while local writes are not fenced");
  if (Status st = txn.DeleteRange(ColumnFamily::kReplog, kWholeCfLo, ""); !st.ok()) return st;
  if (Status st = txn.Delete(ColumnFamily::kSys, kReplFirstKey); !st.ok()) return st;
  if (Status st = txn.Delete(ColumnFamily::kSys, kBaselineUnknownKey); !st.ok()) return st;
  if (Status st = txn.Put(ColumnFamily::kSys, kSysReplLoadIncomplete, "1"); !st.ok()) return st;
  if (Status st = txn.Commit(); !st.ok()) return st;
  SetBaselineLocked(0, 0, 1);
  load_incomplete_.store(true, std::memory_order_release);
  return Status::Ok();
}

Status ReplLog::CommitLoadEnd(WriteTxn& txn, uint64_t seq, std::string_view record) {
  uint64_t cum = 0;
  if (seq == 0 && !record.empty()) return Status::Corruption("full-sync baseline: a record for seq 0");
  if (seq > 0) {
    auto batch = encoding::DecodeReplogBatch(record);
    if (!batch.ok()) return Status::Corruption("full-sync baseline record: " + batch.status().message());
    cum = batch->cum_bytes;
  }
  std::lock_guard lock(mu_);
  if (!fenced()) return Status::Internal("full-sync load while local writes are not fenced");
  if (seq > 0) {
    std::string first;
    coding::PutFixed64BE(&first, seq);
    if (Status st = txn.Put(ColumnFamily::kReplog, encoding::ReplogKey(seq), record); !st.ok()) return st;
    if (Status st = txn.Put(ColumnFamily::kSys, kReplFirstKey, first); !st.ok()) return st;
  }
  if (Status st = txn.Delete(ColumnFamily::kSys, kSysReplLoadIncomplete); !st.ok()) return st;
  if (Status st = txn.Commit(); !st.ok()) return st;
  SetBaselineLocked(seq, cum, seq == 0 ? 1 : seq);
  load_incomplete_.store(false, std::memory_order_release);
  return Status::Ok();
}

// G1 (M6b final review #1): the same log the start txn leaves, whatever happened since (a failed abort
// followed by applied batches, or a restart), so "marker => discard to a consistent seq-0 node" holds
// unconditionally and a discarded node can never ask to continue a history its keyspace lost.
Status ReplLog::CommitLoadAbort(WriteTxn& txn) {
  std::lock_guard lock(mu_);
  if (Status st = txn.DeleteRange(ColumnFamily::kReplog, kWholeCfLo, ""); !st.ok()) return st;
  if (Status st = txn.Delete(ColumnFamily::kSys, kReplFirstKey); !st.ok()) return st;
  if (Status st = txn.Delete(ColumnFamily::kSys, kBaselineUnknownKey); !st.ok()) return st;
  if (Status st = txn.Delete(ColumnFamily::kSys, kSysReplLoadIncomplete); !st.ok()) return st;
  if (Status st = txn.Commit(); !st.ok()) return st;
  SetBaselineLocked(0, 0, 1);
  load_incomplete_.store(false, std::memory_order_release);
  return Status::Ok();
}

}  // namespace tideskv
