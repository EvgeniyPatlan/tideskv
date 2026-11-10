#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "exec/commit_listener.h"
#include "storage/compactor.h"

namespace tideskv {

// Background compaction (M3 plan E6, M2 Task 3 verdict). Classifies committed range deletes:
//  - a 1-byte prefix (FLUSHDB of that db) or a DeleteRange (FLUSHALL) -> compact that whole CF
//    (a range compaction cannot reclaim a FLUSHDB: see TidesStorage::Compact);
//  - a subkey prefix of encoding::ObjectPrefixSize() bytes (a collection deleted: 11 slotted, 9
//    slotless) -> counted; at the threshold the whole subkey and zscore CFs are compacted and the count
//    restarts, provided enough of the CF is dead (Options::dead_ratio). The next collection-triggered compaction waits collection_compaction_spacing times the
//    last subkey compaction's length after it ended (it rewrites the whole CF), so a steady stream of
//    big-collection DELs cannot keep one running for good; FLUSHDB/FLUSHALL jobs are never held back.
// A job is checkpointed once before its first attempt (range tombstones still in the memtable are not
// compacted). A failed job is retried on later ticks up to max_attempts, then counted as failed; Busy
// (another compaction holds the CF) is retried after busy_retry without using an attempt or another
// checkpoint.
// Queued jobs are deduplicated per CF.
// All scheduler state is in memory only: queued compactions are lost on a crash or Stop (a startup
// compaction pass is an M8 item).
class CompactionScheduler final : public CommitListener {
 public:
  struct Options {
    std::chrono::milliseconds tick{200};
    int max_attempts = 5;
    std::function<uint64_t()> collection_delete_threshold = [] { return uint64_t{1000}; };  // 0 = never
    std::chrono::milliseconds busy_retry{2000};
    // A collection-triggered compaction also waits until the deleted elements reach dead_ratio of the
    // subkey CF's sstable keys (subkey_keys). 0 = count and spacing only; subkey_keys() == 0 (all data
    // still in memtables) also falls back to the count.
    double dead_ratio = 0.2;
    std::function<double()> dead_ratio_source;  // when set, read at each check instead of dead_ratio (CONFIG SET)
    std::function<uint64_t()> subkey_keys = [] { return uint64_t{0}; };
    uint64_t collection_compaction_spacing = 4;  // 0 = compact at every threshold
    std::function<std::chrono::steady_clock::time_point()> now = [] { return std::chrono::steady_clock::now(); };
  };

  CompactionScheduler(Compactor& compactor, Options options)
      : compactor_(compactor), options_(std::move(options)) {}
  ~CompactionScheduler() override { Stop(); }
  CompactionScheduler(const CompactionScheduler&) = delete;
  CompactionScheduler& operator=(const CompactionScheduler&) = delete;

  void OnCommitted(const std::vector<RangeDelete>& deletes) override;  // only enqueues
  // Runs the queued jobs on the calling thread; returns how many finished (done or given up).
  size_t RunPending();
  // Start/Stop are not concurrent with each other. Stop drops queued and not-yet-started jobs;
  // a Compact already running is not interrupted.
  void Start();
  void Stop();

  size_t pending() const;
  uint64_t completed() const { return completed_.load(std::memory_order_relaxed); }
  uint64_t failed() const { return failed_.load(std::memory_order_relaxed); }

 private:
  struct Job {
    ColumnFamily cf = ColumnFamily::kMeta;
    int attempts = 0;
    bool checkpointed = false;  // its tombstones are in sstables: a retry needs no new checkpoint
  };
  bool EnoughIsDead(double ratio) const;  // requires mu_
  void EnqueueLocked(Job job);  // requires mu_
  Status RunJob(Job& job);
  void Loop();

  Compactor& compactor_;
  Options options_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> queue_;
  uint64_t collection_deletes_ = 0;  // since the last collection-triggered compaction
  uint64_t deleted_elements_ = 0;    // elements of those collections (RangeDelete::elements)
  std::chrono::steady_clock::time_point collection_not_before_{};  // spacing after the last subkey compaction
  bool last_round_busy_ = false;  // the worker's next wait is busy_retry, not tick
  std::atomic<uint64_t> completed_{0};
  std::atomic<uint64_t> failed_{0};
  std::atomic<bool> stop_{false};  // written under mu_; also read between jobs without it
  std::thread thread_;
};

}  // namespace tideskv
