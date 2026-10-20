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
// sys/repl_head: 16 bytes, BE seq then BE cum_bytes: a dormant log's last and cum, which no record carries.
// Present only while commits went unlogged (it is then ahead of the newest record); activation deletes it.
inline constexpr std::string_view kSysReplHead = "repl_head";

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
// (ActivateAndSnapshot also creates a storage snapshot under it, M6b D2).
// Never call into ReplLog while holding the storage commit mutex.
//
// repl-backlog-lazy (`lazy`): with no replica the log is dormant. Commit still takes seq = last+1 and
// advances cum_bytes by the batch's encoded size, but writes no record; first_retained() stays last()+1,
// so nothing committed while dormant is ever offered for CONTINUE. The first snapshot that needs the log
// (a PSYNC, a slot migration), a replicated batch or a full-sync load activates it; it then stays active,
// across restarts too (a log with records starts active). sys/repl_head keeps last and cum over a restart
// while no record does: the first unlogged commit writes it in that commit's txn, PersistHead refreshes it.
class ReplLog {
 public:
  // lazy = false (tests, repl-backlog-lazy no): always active.
  ReplLog(Storage& storage, const Clock& clock, bool lazy = false)
      : storage_(storage), clock_(clock), lazy_(lazy) {}
  // Reads the last replog entry (seq, cum_bytes), sys/repl_first, sys/repl_head and
  // sys/repl_load_incomplete. Call once before use. last and cum are the head's when it is ahead of the
  // newest record (first is then last + 1). A lazy log starts dormant unless it has records through
  // last; a log that is not lazy activates at once (writing the baseline when commits went unlogged).
  Status Recover();
  // Commits txn; if ops is non-empty first appends replog[last+1] = their batch. kUserError with
  // the READONLY text while local writes are fenced (replica), and with kErrTooLargeToReplicate when the
  // encoded batch exceeds max_batch_bytes() (R12). On failure nothing is consumed.
  // txn must be the underlying transaction, not a RecordingTxn (which refuses the replog CF).
  // Dormant: no record; the first unlogged commit puts sys/repl_head on txn instead.
  Status Commit(WriteTxn& txn, const std::vector<encoding::ReplOp>& ops);
  // OidAllocator and other standalone writers: one small txn of `ops` through Commit.
  Status CommitStandalone(const std::vector<encoding::ReplOp>& ops);
  // Replica apply (D11): txn already holds the batch's ops; puts replog[seq] = payload verbatim and
  // commits. kInternal unless seq == last()+1; kCorruption if payload has no valid header or its
  // cum_bytes is not cum_bytes() + payload.size() (the master's stream does not continue ours).
  // On failure nothing is consumed and txn is left uncommitted. Not refused by the fence: it is the
  // replica's one writer (Engine::ApplyReplicated requires the fence raised). kInternal while
  // load_incomplete() (checked under mu_): batches never extend a log over a partial load (G1).
  // A dormant log is activated first (its baseline in a txn of its own).
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
  // Backlog trimming (D18): one txn range-deletes replog[.. up_to] (from the start of the CF, so an
  // activation's baseline just below first_retained() goes too) and persists
  // sys/repl_first = up_to + 1; it is begun and committed under mu_, so it never interleaves with a
  // commit section, and then first_retained() becomes up_to + 1. Not refused by the fence: a replica's
  // applier trims between batches. InvalidArgument when up_to >= last(): the newest batch is always kept,
  // so last and cum_bytes survive a restart. Nothing to do (Ok) when up_to < first_retained().
  // Conflicts: the txn writes only replog keys below last() and sys/repl_first; a commit section writes
  // replog[last()+1] and keyspace CFs, and only Activate (also under mu_) writes repl_first besides, so
  // the two never conflict.
  Status Trim(uint64_t up_to);
  // The master's trimmer thread: Trim, unless local writes are fenced (checked under mu_), in which case
  // nothing is written and *trimmed is false. Once FenceLocalWrites(true) returns, no such trim commits.
  Status TrimUnlessFenced(uint64_t up_to, bool* trimmed);

  // Activates a dormant log, then takes BeginSnapshot() and last() together under mu_: no commit section
  // runs in between, so the view holds exactly the commits <= seq (spike s_snapcut), and replog[seq]
  // (when seq > 0) for the full sync's LOADEND.
  Result<SyncSnapshot> ActivateAndSnapshot();
  // Under mu_; nothing to do when active. When commits went unlogged, one txn puts the baseline
  // replog[S] (S = last(): no ops, cum_bytes = cum_bytes()), persists sys/repl_first = S + 1 (S itself
  // was not logged, so no replica that lacks it may continue from it), deletes sys/repl_head and the
  // records below S (left from an earlier active period). It writes only keys a commit section never
  // writes, so it cannot conflict with one. Not fence-checked: a demoted node may be promoted later.
  Status Activate();
  bool active() const { return active_.load(std::memory_order_acquire); }
  // Dormant with unlogged commits: puts sys/repl_head = (last, cum) unless it already holds last. Under
  // mu_, so an older head never overwrites a newer one. The trimmer thread calls it every tick, and the
  // app at a clean shutdown; a crash loses at most the commits since (the replid rotates at the next
  // master start, and a replica beyond the history's end gets a full resync).
  Status PersistHead();
  bool load_incomplete() const { return load_incomplete_.load(std::memory_order_acquire); }
  // A replica's full-sync load (M6b D5). Each commits `txn` (already holding that step's keyspace writes)
  // under mu_. Start and End require the fence (kInternal otherwise).
  // Start: + DeleteRange(the whole replog), Delete(sys/repl_first), Delete(sys/repl_baseline_unknown),
  // Delete(sys/repl_head), Put(the marker). Then last = cum = 0, first = 1, and the log is active.
  Status CommitLoadStart(WriteTxn& txn);
  // End: seq == 0 requires an empty record; otherwise the record must decode as a batch (CRC included),
  // then Put(replog[seq] = record), Put(sys/repl_first = seq). Deletes the marker. Then last = seq,
  // cum = the record's cum_bytes (0 when seq == 0), first = seq (1 when seq == 0). Corruption on a bad
  // record, with nothing committed.
  Status CommitLoadEnd(WriteTxn& txn, uint64_t seq, std::string_view record);
  // Abort, and startup recovery of an interrupted load (the caller's txn wipes the keyspace): the start
  // txn's log reset whatever the log holds now: DeleteRange(the whole replog), Delete(sys/repl_first),
  // Delete(sys/repl_baseline_unknown), Delete(sys/repl_head) and Delete(the marker). Then last = cum = 0,
  // first = 1 (G1). Not fence-checked: startup runs it before any role is taken.
  Status CommitLoadAbort(WriteTxn& txn);

 private:
  // Requires mu_: puts replog[seq] = record on txn, commits it and publishes seq.
  Status AppendAndCommit(WriteTxn& txn, uint64_t seq, std::string_view record, uint64_t cum);
  // Requires mu_, dormant: commits txn as seq (of encoded size `size`) without a record.
  Status CommitUnlogged(WriteTxn& txn, uint64_t seq, size_t size);
  Status RecoverLast(ReadView& view, uint64_t* last, uint64_t* cum);
  Status RecoverFirst(ReadView& view, uint64_t last, uint64_t* first);
  Status RecoverLoadMarker(ReadView& view, bool* incomplete);
  Status RecoverHead(ReadView& view, bool* found, uint64_t* seq, uint64_t* cum);
  Status ActivateLocked();            // requires mu_
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
  const bool lazy_;
  std::atomic<bool> active_{true};
  bool unlogged_ = false;  // guarded by mu_: last_ is in no record; sys/repl_head is on disk, past them
  uint64_t head_seq_ = 0;  // guarded by mu_: the seq sys/repl_head holds (meaningful while unlogged_)
};

}  // namespace tideskv
