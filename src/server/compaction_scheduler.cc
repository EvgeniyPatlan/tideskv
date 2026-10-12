#include "server/compaction_scheduler.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>

#include "encoding/key_codec.h"
#include "encoding/object_codec.h"

namespace tideskv {

void CompactionScheduler::OnCommitted(const std::vector<RangeDelete>& deletes) {
  const uint64_t threshold = options_.collection_delete_threshold();  // outside mu_: reads config
  std::lock_guard lock(mu_);
  const size_t before = queue_.size();
  for (const RangeDelete& d : deletes) {
    if (!d.is_prefix) {
      EnqueueLocked({d.cf, {}, {}});
    } else if (d.lo.size() == 1) {
      EnqueueLocked({d.cf, d.lo, encoding::PrefixUpperBound(d.lo)});
    } else if (d.cf == ColumnFamily::kSubkey && d.lo.size() == encoding::kObjectPrefixSize) {
      ++collection_deletes_;
    }
  }
  if (threshold > 0 && collection_deletes_ >= threshold) {
    collection_deletes_ = 0;
    EnqueueLocked({ColumnFamily::kSubkey, {}, {}});
    EnqueueLocked({ColumnFamily::kZScore, {}, {}});
  }
  if (queue_.size() != before) cv_.notify_one();
}

void CompactionScheduler::EnqueueLocked(Job job) {
  for (const Job& q : queue_) {
    if (q.cf == job.cf && (q.whole() || (q.lo == job.lo && q.hi == job.hi))) return;  // already covered
  }
  if (job.whole()) std::erase_if(queue_, [&](const Job& q) { return q.cf == job.cf; });
  queue_.push_back(std::move(job));
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
    const Status st = checkpoint.ok() ? compactor_.CompactRange(job.cf, job.lo, job.hi) : checkpoint;
    if (st.ok()) {
      completed_.fetch_add(1, std::memory_order_relaxed);
      ++finished;
    } else if (++job.attempts < options_.max_attempts) {
      std::lock_guard lock(mu_);
      EnqueueLocked(std::move(job));
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
