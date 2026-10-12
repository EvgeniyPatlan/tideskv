#pragma once
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

#include "common/clock.h"
#include "common/status.h"
#include "exec/gate.h"
#include "exec/lock_manager.h"
#include "storage/storage.h"

namespace tideskv {

// The only path from commands to storage (spec §5.1). M5 swaps in the per-worker
// ExclusiveGate and M6 adds the Sequencer and replog here; command code never changes.
//
// Lock order: the Gate is always taken before any key stripe. The Gate is not re-entrant,
// so a ReadFn/WriteFn must never call back into the Engine (debug builds assert this).
//
// Every fn receives now_ms: the clock read after the Gate (and, for writes, the key stripes)
// is held, and re-read on each commit attempt. Expiry/liveness decisions must use it, never
// a time taken before the call: a key can expire while the command waits for its locks.
class Engine {
 public:
  using ReadFn = std::function<Status(ReadView&, int64_t now_ms)>;
  using WriteFn = std::function<Status(WriteTxn&, int64_t now_ms)>;
  static constexpr int kMaxCommitAttempts = 3;  // spec §5.4: TDB_ERR_CONFLICT

  Engine(Storage& storage, LockManager& locks, const Clock& clock)
      : storage_(storage), locks_(locks), clock_(clock) {}

  // For parse-time work only (turning a relative EX/PX into a deadline, as Redis does at
  // command start); never for an expiry decision, which uses the fn's now_ms.
  int64_t NowMs() const { return clock_.NowMs(); }
  // Latest committed data; no key locks.
  Status Read(const ReadFn& fn);
  // One point-in-time view for commands that read many keys (MGET, DBSIZE, SCAN, KEYS).
  Status Snapshot(const ReadFn& fn);
  // Locks `keys` of `db`, runs fn in one transaction and commits. fn runs again after a
  // commit conflict, so it must reset its outputs first. A non-OK status from fn rolls
  // the transaction back and is returned unchanged.
  Status Write(uint8_t db, const std::vector<std::string_view>& keys, const WriteFn& fn);
  // Whole-keyspace write (FLUSHDB/FLUSHALL): waits for in-flight commands, then runs alone.
  Status WriteExclusive(const WriteFn& fn);

 private:
  Status RunTxn(const WriteFn& fn);

  Storage& storage_;
  LockManager& locks_;
  const Clock& clock_;
  Gate gate_;
};

}  // namespace tideskv
