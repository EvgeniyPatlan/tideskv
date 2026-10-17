#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "common/clock.h"
#include "common/status.h"
#include "exec/commit_listener.h"
#include "exec/gate.h"
#include "exec/lock_manager.h"
#include "exec/recording_txn.h"
#include "exec/repl_log.h"
#include "exec/slot_fence.h"
#include "exec/write_observer.h"
#include "storage/storage.h"

namespace tideskv {

namespace engine_internal {
struct WriteScope;
}

// The only path from commands to storage (spec §5.1). With a ReplLog (M6a D3) every commit goes through
// it, so each transaction that wrote something appends one replog batch of its ops; command code never
// changes.
//
// Lock order: the Gate is always taken before any key stripe. The Gate is not re-entrant,
// so a ReadFn/WriteFn must never call back into the Engine (debug builds assert this).
//
// Every fn receives now_ms: the clock read after the Gate (and, for writes, the key stripes)
// is held, and re-read on each commit attempt. Expiry/liveness decisions must use it, never
// a time taken before the call: a key can expire while the command waits for its locks.
//
// After a successful commit with range deletes, the listener (if any) is called on this thread,
// still under the gate and key stripes.
class Engine {
 public:
  using ReadFn = std::function<Status(ReadView&, int64_t now_ms)>;
  using WriteFn = std::function<Status(WriteTxn&, int64_t now_ms)>;
  static constexpr int kMaxCommitAttempts = 3;  // spec §5.4: TDB_ERR_CONFLICT

  Engine(Storage& storage, LockManager& locks, const Clock& clock, CommitListener* listener = nullptr,
         WriteObserver* observer = nullptr, ReplLog* repl_log = nullptr)
      : storage_(storage),
        locks_(locks),
        clock_(clock),
        listener_(listener),
        observer_(observer),
        repl_log_(repl_log) {}

  ReplLog* repl_log() const { return repl_log_; }  // may be null: nothing is logged

  // Replica apply (M6a D11): applies one master batch atomically as replog[seq] (its ops and the
  // verbatim payload in one transaction), then reports range deletes (listener) and modified keys
  // (observer: every meta key it put or deleted, and the watched keys of a db it flushed) like a
  // local write. Waits through a busy script, as Redis applies the master stream after one.
  // kInternal if seq != repl_log()->last()+1, the engine has no ReplLog, its local writes are not
  // fenced, a full-sync load is running or its marker is set (G1), or this thread is in a batch;
  // kCorruption for a bad payload or a cum_bytes that does not continue ours. On failure nothing is applied.
  // The apply itself bypasses the fence (FenceLocalWrites refuses only local writers) but requires it raised:
  // the gate choice below depends on it.
  //
  // Gate choice: shared, with no key stripes. That is safe because a replica has no other writer:
  // the fenced ReplLog refuses every local commit (commands, the reaper, oid leases), and an EXEC
  // or script on the replica holds the gate exclusively, so its reads stay isolated from applies.
  // Applies must come from one thread, in seq order.
  Status ApplyReplicated(uint64_t seq, std::string_view payload);

  // For parse-time work only (turning a relative EX/PX into a deadline, as Redis does at
  // command start); never for an expiry decision, which uses the fn's now_ms.
  int64_t NowMs() const;
  // Latest committed data; no key locks.
  Status Read(const ReadFn& fn);
  // One point-in-time view for commands that read many keys (MGET, DBSIZE, SCAN, KEYS).
  Status Snapshot(const ReadFn& fn);
  // Locks `keys` of `db`, runs fn in one transaction and commits. fn runs again after a
  // commit conflict, so it must reset its outputs first (the Engine clears *also_modified for it).
  // A non-OK status from fn rolls the transaction back and is returned unchanged.
  // After the commit, still under the locks, the observer hears which keys changed (M5a plan G2):
  // every key whose meta record fn put or deleted; for a one-key write, that key if fn wrote
  // anything; and the keys fn appended to *also_modified (Redis signals some no-op writes).
  Status Write(uint8_t db, const std::vector<std::string_view>& keys, const WriteFn& fn,
               std::vector<std::string>* also_modified = nullptr);
  // Whole-keyspace write (FLUSHDB/FLUSHALL): waits for in-flight commands, then runs alone.
  // `flushed_dbs`: the dbs fn empties; their watched keys that exist before fn runs are reported.
  Status WriteExclusive(const WriteFn& fn, const std::vector<uint8_t>& flushed_dbs = {});

  // An atomic batch (EXEC; M5a plan G1). Holds the gate exclusively and one transaction; the
  // clock is frozen. Until it is destroyed or committed, every Engine call on the creating
  // thread runs inside that transaction without locking, each Write under its own savepoint
  // (rolled back if fn fails). Destroying it without Commit rolls everything back.
  class Batch {
   public:
    ~Batch();
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;
    int64_t now_ms() const { return now_ms_; }
    // Commits, then tells the listener and the observer, still under the gate. At most once;
    // routing stops either way. On failure nothing was applied. The thread still holds the gate
    // until the Batch is destroyed, so make no Engine call on it in between (it would deadlock).
    Status Commit();

   private:
    friend class Engine;
    Result<uint64_t> Mark();            // savepoint at the current write position; the mark id
    Status RollbackTo(uint64_t mark);   // undoes the writes (and their modified keys) after the mark
    Status ReleaseMark(uint64_t mark);  // forgets the newest mark; its writes stay
    Batch(Engine& engine, std::unique_lock<Gate> gate, std::unique_ptr<WriteTxn> txn, int64_t now_ms);
    Status RunRead(const ReadFn& fn);
    Status RunWrite(const WriteFn& fn, const engine_internal::WriteScope& scope);
    void UndoWrite(const std::string& savepoint);  // rolls back to and releases a write's savepoint
    void Release(const std::string& savepoint);    // a failure only warns: a leftover costs speed
    void Unroute();
    Status CommitTxn();  // through the ReplLog when there is one

    Engine& engine_;
    std::unique_lock<Gate> gate_;    // first: released last, after the txn is gone
    std::unique_ptr<WriteTxn> txn_;  // before recorder_, which refers to it
    RecordingTxn recorder_;
    int64_t now_ms_;
    uint64_t next_savepoint_ = 0;
    std::vector<DbKey> modified_;
    struct MarkPoint {
      uint64_t savepoint;  // unique savepoint number: a mark id is reused after RollbackTo
      size_t modified;     // modified_.size() at Mark()
    };
    std::vector<MarkPoint> marks_;  // index = mark id; RollbackTo(m) forgets the marks after m
    Status broken_;                 // a savepoint rollback failed: the txn may hold half a command
    bool done_ = false;
  };
  // Fails with kInternal if this thread already has a batch (G1).
  Result<std::unique_ptr<Batch>> BeginBatch();
  bool InBatch() const { return ActiveBatch() != nullptr; }
  // Script-level undo inside the active batch (M5b H3): BatchRollbackTo(mark) undoes every write
  // made after BatchMark() and forgets their modified keys. BatchReleaseMark(mark) drops the newest
  // mark once it is no longer needed (after a rollback to it too), keeping its writes; release every
  // mark, since each live one slows down later writes of the batch. kInternal outside a batch, and
  // for an unknown, released, undone or not-newest mark.
  Result<uint64_t> BatchMark();
  Status BatchRollbackTo(uint64_t mark);
  Status BatchReleaseMark(uint64_t mark);

  // M5b H6: while busy() holds, a call that would wait for the gate returns UserError(busy_reply)
  // instead. busy must be cheap and must not touch the Engine; whoever makes it true then calls
  // WakeWaiters() so calls already waiting re-check it. Set before the Engine is shared.
  void SetBusyGuard(std::function<bool()> busy, std::string busy_reply);
  void WakeWaiters() { gate_.Wake(); }  // takes only the gate's internal mutex, never the gate
  // True iff `st` is the busy answer above (and not another user error).
  bool IsBusyAnswer(const Status& st) const {
    return busy_ && st.code() == StatusCode::kUserError && st.message() == busy_reply_;
  }

  // While one is alive on this thread, its Engine calls ignore the busy guard: they wait for the gate,
  // through a timed-out script, as Redis serves blocked clients after the script. For work that must not
  // fail with BUSY (ServeBlockedClients). The thread must hold no lock a script can need while it holds
  // the gate. Scopes nest; each restores the previous state.
  class WaitThroughBusyScope {
   public:
    WaitThroughBusyScope();
    ~WaitThroughBusyScope();
    WaitThroughBusyScope(const WaitThroughBusyScope&) = delete;
    WaitThroughBusyScope& operator=(const WaitThroughBusyScope&) = delete;

   private:
    bool previous_;
  };

  // ---- A replica's full-sync load (M6b D5, D6) ----
  // True from BeginLoad until EndLoad/AbortLoad. Meanwhile Read/Snapshot/Write/WriteExclusive answer
  // UserError(kErrLoading), checked once the gate is held (so a command that passed dispatch just before
  // the load started is still refused), except Read/Snapshot on a thread inside a LoadingReadScope.
  // BeginBatch is not refused (PF7, as Redis runs an EXEC whose queue touches no data); the batch's own
  // keyspace access is, under its exclusive gate. The steps are the replica's one committer's (its client
  // thread), in order: BeginLoad, LoadChunk*, then EndLoad or AbortLoad.
  bool loading() const { return loading_.load(std::memory_order_acquire); }
  // Raises loading while holding the gate exclusively (every command already holding it finishes first),
  // then commits one txn: the keyspace wipe + `extra` (the node's identity) through
  // ReplLog::CommitLoadStart; tells the observer the dataset is replaced. kInternal unless the ReplLog is
  // fenced, this thread has no batch and no load is running. On failure loading is cleared and nothing
  // changed.
  Status BeginLoad(const std::vector<encoding::ReplOp>& extra);
  // One txn of `ops` (puts from the master's snapshot), committed directly: no replog entry. kInternal
  // unless loading.
  Status LoadChunk(const std::vector<encoding::ReplOp>& ops);
  // The last txn: `extra` + ReplLog::CommitLoadEnd(seq, record). Then reports the wipe's range deletes to
  // the listener, tells the observer, and clears loading. kInternal unless a BeginLoad succeeded and no
  // EndLoad/AbortLoad followed (CF-M6b-2). On failure loading stays set (AbortLoad next).
  Status EndLoad(uint64_t seq, std::string_view record, const std::vector<encoding::ReplOp>& extra);
  // Discards whatever was loaded: one txn of the keyspace wipe + ReplLog::CommitLoadAbort; reports,
  // tells the observer, clears loading. Also run at startup when ReplLog::load_incomplete(). kInternal
  // when there is nothing to discard (neither loading nor load_incomplete()): it never wipes a dataset.
  Status AbortLoad();
  // DEBUG (CF6): Read/Snapshot on this thread are not refused while loading. Scopes nest.
  class LoadingReadScope {
   public:
    LoadingReadScope();
    ~LoadingReadScope();
    LoadingReadScope(const LoadingReadScope&) = delete;
    LoadingReadScope& operator=(const LoadingReadScope&) = delete;

   private:
    bool previous_;
  };

  // ---- The slot write fence of a migrating source (M7b D4) ----
  // Each change takes the gate exclusively, the barrier BeginLoad uses: every write already holding the gate
  // commits (or fails) first, and every later Write/RunWrite sees the new fence and answers kErrTryAgain for
  // a fenced key. Returns repl_log()->last() read inside that barrier (0 without a log): no write to a newly
  // fenced slot can commit after it, so a migrator ships its final delta through exactly that seq. Waits
  // through a busy script, as BeginLoad (D24.2). Never from inside an Engine fn or with a batch open on this
  // thread (it would deadlock on the gate). Unfenced: ApplyReplicated, loads, ApplyImport, WriteInternal,
  // WriteExclusive (FLUSH*, Writer's note 1), oid leases.
  uint64_t FenceSlots(uint16_t first, uint16_t last);
  uint64_t UnfenceSlots(uint16_t first, uint16_t last);
  uint64_t AssignFence(const SlotBits& bits);  // the fence becomes exactly `bits`
  bool SlotFenced(uint16_t slot) const { return fence_.Test(slot); }
  SlotBits FencedSlots() const { return fence_.Bits(); }

 private:
  // M6b D6: whether a call that now holds the gate is refused because a load runs.
  bool LoadingRefuses(bool read) const;
  // One load txn under the shared gate: `ops` in order, committed by `commit`; on success `committed`
  // runs still under the gate (listener/observer contracts).
  Status RunLoadTxn(const std::vector<encoding::ReplOp>& ops, const std::function<Status(WriteTxn&)>& commit,
                    const std::function<void()>& committed);
  void FinishLoad();         // under the gate, after EndLoad/AbortLoad committed
  bool AcquireShared();      // false: gave up because busy_
  bool AcquireExclusive();   // false: gave up because busy_
  bool BusyGuarded() const;  // busy_ is set and no WaitThroughBusyScope is alive on this thread
  Status BusyStatus() const { return Status::UserError(busy_reply_); }
  Batch* ActiveBatch() const;
  Status RunTxn(const WriteFn& fn, const engine_internal::WriteScope& scope);
  // Flushed-db keys to report (G2); empty unless scope.flushed_dbs and a non-idle observer.
  Status CollectFlushed(ReadView& view, const engine_internal::WriteScope& scope, std::vector<DbKey>* out);
  bool Observing() const { return observer_ != nullptr && !observer_->Idle(); }
  void Report(const std::vector<DbKey>& keys);                       // under the gate, after commit
  uint64_t ChangeFence(const std::function<void(SlotBits&)>& edit);  // under the exclusive barrier

  Storage& storage_;
  LockManager& locks_;
  const Clock& clock_;
  CommitListener* listener_;  // may be null; told about committed range deletes
  WriteObserver* observer_;   // may be null; told which keys each commit modified
  ReplLog* repl_log_;         // may be null; sequences every commit into the replog
  Gate gate_;
  std::atomic<bool> loading_{false};  // M6b D6: raised under the exclusive gate
  std::function<bool()> busy_;        // empty: plain gate waits
  std::string busy_reply_;
  SlotFence fence_;  // M7b D4: changed only under the exclusive gate
};

}  // namespace tideskv
