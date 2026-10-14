#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <vector>

#include "common/clock.h"
#include "common/status.h"
#include "encoding/replog_codec.h"
#include "storage/storage.h"

namespace tideskv {

// The replication log (M6a D3): the only writer of the replog CF. Commit() serialises
// "seq = last+1 → put replog[seq] → commit → publish" so seq order is commit order and the
// watermark (last committed seq) has no gaps. Lock order: taken after the gate, key stripes,
// ScriptEngine::mu_ and WatchRegistry shards; it takes only the storage commit mutex beneath it.
// Never call into ReplLog while holding the storage commit mutex.
class ReplLog {
 public:
  ReplLog(Storage& storage, const Clock& clock) : storage_(storage), clock_(clock) {}
  // Reads the last replog entry (seq, cum_bytes) and sys/repl_first. Call once before use.
  Status Recover();
  // Commits txn; if ops is non-empty first appends replog[last+1] = their batch. kUserError with
  // the READONLY text while local writes are fenced (replica). On failure nothing is consumed.
  // txn must be the underlying transaction, not a RecordingTxn (which refuses the replog CF).
  Status Commit(WriteTxn& txn, const std::vector<encoding::ReplOp>& ops);
  // OidAllocator and other standalone writers: one small txn of `ops` through Commit.
  Status CommitStandalone(const std::vector<encoding::ReplOp>& ops);
  // Replica apply (D11): txn already holds the batch's ops; puts replog[seq] = payload verbatim and
  // commits. kInternal unless seq == last()+1; kCorruption if payload has no valid header. Never fenced.
  Status CommitReplicated(WriteTxn& txn, uint64_t seq, std::string_view payload);
  uint64_t last() const { return last_.load(std::memory_order_acquire); }             // watermark
  uint64_t first_retained() const { return first_.load(std::memory_order_acquire); }  // 1 if nothing trimmed
  uint64_t cum_bytes() const;  // cum_bytes of last(); 0 if empty
  // Blocks until last() > seq or the timeout passes; returns last().
  uint64_t WaitBeyond(uint64_t seq, std::chrono::milliseconds timeout) const;
  void FenceLocalWrites(bool fenced) { fenced_.store(fenced, std::memory_order_release); }
  bool fenced() const { return fenced_.load(std::memory_order_acquire); }
  // Backlog trimming (Task 14) updates first_retained after its own trim txn commits.
  void SetFirstRetained(uint64_t first);

 private:
  // Requires mu_: puts replog[seq] = record on txn, commits it and publishes seq.
  Status AppendAndCommit(WriteTxn& txn, uint64_t seq, std::string_view record, uint64_t cum);
  Status RecoverLast(ReadView& view, uint64_t* last, uint64_t* cum);
  Status RecoverFirst(ReadView& view, uint64_t last, uint64_t* first);

  Storage& storage_;
  const Clock& clock_;
  mutable std::mutex mu_;  // the commit section
  mutable std::condition_variable advanced_;
  std::atomic<uint64_t> last_{0};
  std::atomic<uint64_t> first_{1};
  uint64_t cum_ = 0;  // guarded by mu_
  std::atomic<bool> fenced_{false};
};

}  // namespace tideskv
