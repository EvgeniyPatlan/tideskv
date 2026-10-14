#pragma once
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
  // fenced, or this thread is in a batch; kCorruption for a bad payload or a cum_bytes that does not
  // continue ours. On failure nothing is applied. The apply itself bypasses the fence (FenceLocalWrites
  // refuses only local writers) but requires it raised: the gate choice below depends on it.
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

 private:
  bool AcquireShared();      // false: gave up because busy_
  bool AcquireExclusive();   // false: gave up because busy_
  bool BusyGuarded() const;  // busy_ is set and no WaitThroughBusyScope is alive on this thread
  Status BusyStatus() const { return Status::UserError(busy_reply_); }
  Batch* ActiveBatch() const;
  Status RunTxn(const WriteFn& fn, const engine_internal::WriteScope& scope);
  // Flushed-db keys to report (G2); empty unless scope.flushed_dbs and a non-idle observer.
  Status CollectFlushed(ReadView& view, const engine_internal::WriteScope& scope, std::vector<DbKey>* out);
  bool Observing() const { return observer_ != nullptr && !observer_->Idle(); }
  void Report(const std::vector<DbKey>& keys);  // under the gate, after commit

  Storage& storage_;
  LockManager& locks_;
  const Clock& clock_;
  CommitListener* listener_;  // may be null; told about committed range deletes
  WriteObserver* observer_;   // may be null; told which keys each commit modified
  ReplLog* repl_log_;         // may be null; sequences every commit into the replog
  Gate gate_;
  std::function<bool()> busy_;  // empty: plain gate waits
  std::string busy_reply_;
};

}  // namespace tideskv
