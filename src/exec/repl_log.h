#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

#include "common/clock.h"
#include "common/status.h"
#include "encoding/replog_codec.h"
#include "storage/storage.h"

namespace tideskv {

// sys/repl_load_incomplete: present from the first to the last transaction of a replica's full-sync load
// (M6b D5); node-local. Recover() reports it so that startup discards the partial keyspace.
inline constexpr std::string_view kSysReplLoadIncomplete = "repl_load_incomplete";

// A frozen view of the whole store and the watermark it holds exactly (M6b D2).
struct SyncSnapshot {
  std::unique_ptr<ReadView> view;
  uint64_t seq = 0;
};

// Applies `ops` to txn in order, exactly as recorded (M6a D5/D11). Stops at the first failure.
Status ApplyReplOps(WriteTxn& txn, const std::vector<encoding::ReplOp>& ops);

// The replication log (M6a D3): the only writer of the replog CF. Commit() serialises
// "seq = last+1 → put replog[seq] → commit → publish" so seq order is commit order and the
// watermark (last committed seq) has no gaps. Lock order: taken after the gate, key stripes,
// ScriptEngine::mu_ and WatchRegistry shards; it takes only the storage commit mutex beneath it
// (SnapshotAtLast also creates a storage snapshot under it, M6b D2).
// Never call into ReplLog while holding the storage commit mutex.
class ReplLog {
 public:
  ReplLog(Storage& storage, const Clock& clock) : storage_(storage), clock_(clock) {}
  // Reads the last replog entry (seq, cum_bytes), sys/repl_first and sys/repl_load_incomplete. Call once
  // before use.
  Status Recover();
  // Commits txn; if ops is non-empty first appends replog[last+1] = their batch. kUserError with
  // the READONLY text while local writes are fenced (replica), and with kErrTooLargeToReplicate when the
  // encoded batch exceeds max_batch_bytes() (R12). On failure nothing is consumed.
  // txn must be the underlying transaction, not a RecordingTxn (which refuses the replog CF).
  Status Commit(WriteTxn& txn, const std::vector<encoding::ReplOp>& ops);
  // OidAllocator and other standalone writers: one small txn of `ops` through Commit.
  Status CommitStandalone(const std::vector<encoding::ReplOp>& ops);
  // Replica apply (D11): txn already holds the batch's ops; puts replog[seq] = payload verbatim and
  // commits. kInternal unless seq == last()+1; kCorruption if payload has no valid header or its
  // cum_bytes is not cum_bytes() + payload.size() (the master's stream does not continue ours).
  // On failure nothing is consumed and txn is left uncommitted. Not refused by the fence: it is the
  // replica's one writer (Engine::ApplyReplicated requires the fence raised).
  Status CommitReplicated(WriteTxn& txn, uint64_t seq, std::string_view payload);
  uint64_t last() const { return last_.load(std::memory_order_acquire); }             // watermark
  uint64_t first_retained() const { return first_.load(std::memory_order_acquire); }  // 1 if nothing trimmed
  uint64_t cum_bytes() const;  // cum_bytes of last(); 0 if empty
  // Blocks until last() > seq or the timeout passes; returns last().
  uint64_t WaitBeyond(uint64_t seq, std::chrono::milliseconds timeout) const;
  // Raising the fence is a barrier (CF1): it waits out a local commit already inside the commit section,
  // and Commit checks the fence under the same lock, so once this returns no local commit takes a seq.
  void FenceLocalWrites(bool fenced);
  bool fenced() const { return fenced_.load(std::memory_order_acquire); }
  // R12: Commit refuses a batch whose encoding exceeds this (encoding::kMaxReplBatchBytes). Tests lower it.
  void SetMaxBatchBytes(size_t bytes) { max_batch_bytes_.store(bytes, std::memory_order_relaxed); }
  size_t max_batch_bytes() const { return max_batch_bytes_.load(std::memory_order_relaxed); }
  // Backlog trimming (Task 14) updates first_retained after its own trim txn commits.
  void SetFirstRetained(uint64_t first);
  // Backlog trimming (D18): one txn range-deletes replog[first_retained() .. up_to] and persists
  // sys/repl_first = up_to + 1; it is begun and committed under mu_, so it never interleaves with a
  // commit section, and then first_retained() becomes up_to + 1. Not refused by the fence: a replica's
  // applier trims between batches. InvalidArgument when up_to >= last(): the newest batch is always kept,
  // so last and cum_bytes survive a restart. Nothing to do (Ok) when up_to < first_retained().
  // Conflicts: the txn writes only replog keys below last() and sys/repl_first; a commit section writes
  // replog[last()+1] and keyspace CFs, and nothing else writes repl_first, so the two never conflict.
  Status Trim(uint64_t up_to);
  // The master's trimmer thread: Trim, unless local writes are fenced (checked under mu_), in which case
  // nothing is written and *trimmed is false. Once FenceLocalWrites(true) returns, no such trim commits.
  Status TrimUnlessFenced(uint64_t up_to, bool* trimmed);

  // Takes BeginSnapshot() and last() together under mu_: no commit section runs in between, so the view
  // holds exactly the commits <= seq (spike s_snapcut).
  Result<SyncSnapshot> SnapshotAtLast();
  bool load_incomplete() const { return load_incomplete_.load(std::memory_order_acquire); }
  // A replica's full-sync load (M6b D5). Each commits `txn` (already holding that step's keyspace writes)
  // under mu_. Start and End require the fence (kInternal otherwise).
  // Start: + DeleteRange(the whole replog), Delete(sys/repl_first), Delete(sys/repl_baseline_unknown),
  // Put(the marker). Then last = cum = 0, first = 1.
  Status CommitLoadStart(WriteTxn& txn);
  // End: seq == 0 requires an empty record; otherwise the record must decode as a batch (CRC included),
  // then Put(replog[seq] = record), Put(sys/repl_first = seq). Deletes the marker. Then last = seq,
  // cum = the record's cum_bytes (0 when seq == 0), first = seq (1 when seq == 0). Corruption on a bad
  // record, with nothing committed.
  Status CommitLoadEnd(WriteTxn& txn, uint64_t seq, std::string_view record);
  // Abort, and startup recovery of an interrupted load: deletes the marker (the caller's txn wipes the
  // keyspace). Not fence-checked: startup runs it before any role is taken.
  Status CommitLoadAbort(WriteTxn& txn);

 private:
  // Requires mu_: puts replog[seq] = record on txn, commits it and publishes seq.
  Status AppendAndCommit(WriteTxn& txn, uint64_t seq, std::string_view record, uint64_t cum);
  Status RecoverLast(ReadView& view, uint64_t* last, uint64_t* cum);
  Status RecoverFirst(ReadView& view, uint64_t last, uint64_t* first);
  Status RecoverLoadMarker(ReadView& view, bool* incomplete);
  Status TrimLocked(uint64_t up_to);  // requires mu_
  // Requires mu_: the in-memory log after a load step committed.
  void SetBaselineLocked(uint64_t last, uint64_t cum, uint64_t first);

  Storage& storage_;
  const Clock& clock_;
  mutable std::mutex mu_;  // the commit section
  mutable std::condition_variable advanced_;
  std::atomic<uint64_t> last_{0};
  std::atomic<uint64_t> first_{1};
  uint64_t cum_ = 0;  // guarded by mu_
  std::atomic<bool> fenced_{false};
  std::atomic<bool> load_incomplete_{false};
  std::atomic<size_t> max_batch_bytes_{encoding::kMaxReplBatchBytes};
};

}  // namespace tideskv
