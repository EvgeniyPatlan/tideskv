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
//  - an 11-byte subkey prefix (collection deleted) -> counted; at the threshold the whole subkey
//    and zscore CFs are compacted and the count restarts.
// Each batch of jobs starts with one Checkpoint (range tombstones still in the memtable are not
// compacted). A failed job is retried on later ticks up to max_attempts, then counted as failed.
// Queued jobs are deduplicated per CF.
// All scheduler state is in memory only: queued compactions are lost on a crash or Stop (a startup
// compaction pass is an M8 item).
class CompactionScheduler final : public CommitListener {
 public:
  struct Options {
    std::chrono::milliseconds tick{200};
    int max_attempts = 5;
    std::function<uint64_t()> collection_delete_threshold = [] { return uint64_t{1000}; };  // 0 = never
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
  };
  void EnqueueLocked(Job job);  // requires mu_
  void Loop();

  Compactor& compactor_;
  Options options_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> queue_;
  uint64_t collection_deletes_ = 0;  // since the last collection-triggered compaction
  std::atomic<uint64_t> completed_{0};
  std::atomic<uint64_t> failed_{0};
  std::atomic<bool> stop_{false};  // written under mu_; also read between jobs without it
  std::thread thread_;
};

}  // namespace tideskv
