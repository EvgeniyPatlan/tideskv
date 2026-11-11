#include "server/compaction_scheduler.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>

#include "encoding/object_codec.h"

namespace tideskv {

void CompactionScheduler::OnCommitted(const std::vector<RangeDelete>& deletes) {
  const uint64_t threshold = options_.collection_delete_threshold();  // outside mu_: reads config
  const double ratio = options_.dead_ratio();
  std::lock_guard lock(mu_);
  const size_t before = queue_.size();
  for (const RangeDelete& d : deletes) {
    if (!d.is_prefix || d.lo.size() == 1) {
      EnqueueLocked({d.cf});
    } else if (d.cf == ColumnFamily::kSubkey && d.lo.size() == encoding::ObjectPrefixSize()) {
      ++collection_deletes_;
      deleted_elements_ += d.elements;
      if (d.elements == 0) unknown_elements_ = true;  // replicated, imported or rehomed: not hinted
    }
  }
  if (threshold > 0 && collection_deletes_ >= threshold && options_.now() >= collection_not_before_) {
    if (ratio <= 0 || !options_.subkey_keys) {
      EnqueueCollectionsLocked();
    } else {
      eval_pending_ = true;  // the worker reads the key count: storage stats never run on the commit path
    }
  }
  if (queue_.size() != before) cv_.notify_one();
}

void CompactionScheduler::EnqueueCollectionsLocked() {
  collection_deletes_ = 0;
  deleted_elements_ = 0;
  unknown_elements_ = false;
  eval_pending_ = false;
  EnqueueLocked({ColumnFamily::kSubkey});
  EnqueueLocked({ColumnFamily::kZScore});
}

void CompactionScheduler::EvaluateCollectionTrigger() {
  {
    std::lock_guard lock(mu_);
    if (!eval_pending_) return;
    eval_pending_ = false;
  }
  const uint64_t threshold = options_.collection_delete_threshold();
  const double ratio = options_.dead_ratio();
  const uint64_t keys = options_.subkey_keys ? options_.subkey_keys() : 0;  // storage stats, outside mu_
  std::lock_guard lock(mu_);
  if (threshold == 0 || collection_deletes_ < threshold || options_.now() < collection_not_before_) return;
  // keys == 0: nothing in sstables to measure against, so the count decides
  // unknown element counts or a long deferral: the count decides too
  const bool ceiling = collection_deletes_ >= threshold * kMaxDeferralFactor;
  if (!ceiling && !unknown_elements_ && ratio > 0 && keys > 0 &&
      static_cast<double>(deleted_elements_) < ratio * static_cast<double>(keys)) {
    return;
  }
  EnqueueCollectionsLocked();
  cv_.notify_one();
}

void CompactionScheduler::EnqueueLocked(Job job) {
  if (std::any_of(queue_.begin(), queue_.end(), [&](const Job& q) { return q.cf == job.cf; })) return;
  queue_.push_back(job);
}

size_t CompactionScheduler::RunPending() {
  std::deque<Job> jobs;
  {
    std::lock_guard lock(mu_);
    jobs.swap(queue_);
  }
  if (jobs.empty()) return 0;
  // One checkpoint for the jobs that never had one. Retrying a Busy job must not checkpoint again: each
  // checkpoint rotates the memtable, and a rotation every retry tick flooded L1 with tiny flushes.
  Status checkpoint = Status::Ok();
  if (std::any_of(jobs.begin(), jobs.end(), [](const Job& j) { return !j.checkpointed; })) {
    checkpoint = compactor_.Checkpoint();
    if (checkpoint.ok()) {
      for (Job& job : jobs) job.checkpointed = true;
    }
  }
  size_t finished = 0;
  bool busy = false;
  for (Job& job : jobs) {
    if (stop_.load()) break;  // shutting down: drop the rest (neither completed nor failed)
    const Status st = job.checkpointed ? RunJob(job) : checkpoint;
    if (st.ok()) {
      completed_.fetch_add(1, std::memory_order_relaxed);
      ++finished;
    } else if (st.code() == StatusCode::kBusy) {
      busy = true;
      // Another compaction (TidesDB's own, usually) holds the CF. It ends by itself however long a big CF
      // or a slow machine makes it, so it is waited out on later ticks: never an attempt, never a failure.
      std::lock_guard lock(mu_);
      EnqueueLocked(job);
    } else if (++job.attempts < options_.max_attempts) {
      std::lock_guard lock(mu_);
      EnqueueLocked(job);
    } else {
      spdlog::warn("background compaction of CF {} gave up after {} attempts: {}", ColumnFamilyName(job.cf),
                   job.attempts, st.ToString());
      failed_.fetch_add(1, std::memory_order_relaxed);
      ++finished;
    }
  }
  std::lock_guard lock(mu_);
  last_round_busy_ = busy;
  return finished;
}

// Compacts one job's CF. A finished subkey compaction (the collection-delete one rewrites the whole CF)
// sets when the next collection-triggered one may start.
Status CompactionScheduler::RunJob(Job& job) {
  const auto start = options_.now();
  Status st = compactor_.Compact(job.cf);
  if (st.ok() && job.cf == ColumnFamily::kSubkey) {
    const auto end = options_.now();
    std::lock_guard lock(mu_);
    collection_not_before_ = end + (end - start) * options_.collection_compaction_spacing;
  }
  return st;
}

size_t CompactionScheduler::pending() const {
  std::lock_guard lock(mu_);
  return queue_.size();
}

void CompactionScheduler::Start() {
  assert(!thread_.joinable());
  {
    std::lock_guard lock(mu_);
    stop_ = false;
  }
  thread_ = std::thread([this] { Loop(); });
}

void CompactionScheduler::Stop() {
  {
    std::lock_guard lock(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void CompactionScheduler::Loop() {
  std::unique_lock lock(mu_);
  while (!stop_) {
    cv_.wait_for(lock, options_.tick, [this] { return stop_ || !queue_.empty(); });
    if (stop_) continue;
    lock.unlock();
    EvaluateCollectionTrigger();
    lock.lock();
    if (queue_.empty()) continue;
    lock.unlock();
    RunPending();
    lock.lock();
    // pace retries of failed jobs; a Busy CF is held by a compaction that can run for a minute
    cv_.wait_for(lock, last_round_busy_ ? options_.busy_retry : options_.tick,
                 [this] { return stop_.load(); });
  }
}

}  // namespace tideskv
