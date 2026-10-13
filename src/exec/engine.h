#pragma once
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

#include "common/clock.h"
#include "common/status.h"
#include "exec/commit_listener.h"
#include "exec/gate.h"
#include "exec/lock_manager.h"
#include "exec/write_observer.h"
#include "storage/storage.h"

namespace tideskv {

namespace engine_internal {
struct WriteScope;
}

// The only path from commands to storage (spec §5.1). M5 swaps in the per-worker
// ExclusiveGate and M6 adds the Sequencer and replog here; command code never changes.
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
         WriteObserver* observer = nullptr)
      : storage_(storage), locks_(locks), clock_(clock), listener_(listener), observer_(observer) {}

  // For parse-time work only (turning a relative EX/PX into a deadline, as Redis does at
  // command start); never for an expiry decision, which uses the fn's now_ms.
  int64_t NowMs() const { return clock_.NowMs(); }
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

 private:
  Status RunTxn(const WriteFn& fn, const engine_internal::WriteScope& scope);
  // Flushed-db keys to report (G2); empty unless scope.flushed_dbs and a non-idle observer.
  Status CollectFlushed(ReadView& view, const engine_internal::WriteScope& scope, std::vector<DbKey>* out);
  void Report(const std::vector<DbKey>& keys);  // under the gate, after commit

  Storage& storage_;
  LockManager& locks_;
  const Clock& clock_;
  CommitListener* listener_;  // may be null; told about committed range deletes
  WriteObserver* observer_;   // may be null; told which keys each commit modified
  Gate gate_;
};

}  // namespace tideskv
