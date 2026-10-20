#include "server/compaction_scheduler.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>

#include "encoding/object_codec.h"

namespace tideskv {

void CompactionScheduler::OnCommitted(const std::vector<RangeDelete>& deletes) {
  const uint64_t threshold = options_.collection_delete_threshold();  // outside mu_: reads config
  std::lock_guard lock(mu_);
  const size_t before = queue_.size();
  for (const RangeDelete& d : deletes) {
    if (!d.is_prefix || d.lo.size() == 1) {
      EnqueueLocked({d.cf});
    } else if (d.cf == ColumnFamily::kSubkey && d.lo.size() == encoding::ObjectPrefixSize()) {
      ++collection_deletes_;
    }
  }
  if (threshold > 0 && collection_deletes_ >= threshold) {
    collection_deletes_ = 0;
    EnqueueLocked({ColumnFamily::kSubkey});
    EnqueueLocked({ColumnFamily::kZScore});
  }
  if (queue_.size() != before) cv_.notify_one();
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
  const Status checkpoint = compactor_.Checkpoint();
  size_t finished = 0;
  for (Job& job : jobs) {
    if (stop_.load()) break;  // shutting down: drop the rest (neither completed nor failed)
    const Status st = checkpoint.ok() ? compactor_.Compact(job.cf) : checkpoint;
    if (st.ok()) {
      completed_.fetch_add(1, std::memory_order_relaxed);
      ++finished;
    } else if (st.code() == StatusCode::kBusy) {
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
  return finished;
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
    if (stop_ || queue_.empty()) continue;
    lock.unlock();
    RunPending();
    lock.lock();
    cv_.wait_for(lock, options_.tick, [this] { return stop_.load(); });  // pace retries of failed jobs
  }
}

}  // namespace tideskv
