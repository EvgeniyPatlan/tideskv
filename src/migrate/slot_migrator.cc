#include "migrate/slot_migrator.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "cluster/cluster_errors.h"
#include "migrate/migrate_frames.h"
#include "migrate/migration_service.h"
#include "repl/backlog_trimmer.h"
#include "repl/full_sync_frames.h"
#include "repl/replog_reader.h"
#include "types/slot_keys.h"

namespace tideskv::migrate {
namespace {

using namespace std::chrono_literals;

constexpr int kMaxCatchUpRounds = 10;           // Kvrocks kMaxLoopTimes (slot_migrate.cc:655)
constexpr size_t kReadBytes = size_t{1} << 20;  // replog payload read per ReplogReader round
constexpr uint64_t kNotRefenced = UINT64_MAX;   // the fence Mutate found the range fenced already
constexpr size_t kFrameOverhead = 40;           // *3 $7 CLUSTER $10 IMPORTDATA $<n>: the rate limit counts it

Status Err(std::string_view text) { return Status::UserError(std::string(text)); }

// "<prefix><detail>"; a cancellation keeps its own text.
Status Stage(std::string_view prefix, const Status& st) {
  if (st.code() == StatusCode::kUserError && st.message() == cluster::kErrMigrationCanceled) return st;
  return Status::UserError(std::string(prefix) + st.message());
}

}  // namespace

SlotMigrator::SlotMigrator(MigrationService& service, TransportFactory connect,
                           std::function<MigrateSettings()> settings)
    : service_(service),
      connect_(std::move(connect)),
      settings_(std::move(settings)),
      next_seq_(repl::kNoReplicaNeeds) {}

SlotMigrator::~SlotMigrator() { Stop(); }

Status SlotMigrator::Canceled() const {
  return cancel_.load(std::memory_order_acquire) ? Err(cluster::kErrMigrationCanceled) : Status::Ok();
}

Status SlotMigrator::Start(MigrateJob job, MigrateDone done) {
  std::lock_guard lock(job_mu_);
  if (stopped_.load()) return Err(cluster::kErrMigrationCanceled);
  if (running_.load()) return Err(cluster::kErrAlreadyMigrating);
  if (thread_.joinable()) {  // the previous job published its outcome and is in `done` at most
    if (thread_.get_id() == std::this_thread::get_id()) {
      thread_.detach();  // a Start from that `done`: the thread ends right after it
    } else {
      thread_.join();
    }
  }
  cancel_.store(false, std::memory_order_release);
  import_started_ = false;
  running_.store(true, std::memory_order_release);
  thread_ = std::thread([this, job = std::move(job), done = std::move(done)] { Run(job, done); });
  return Status::Ok();
}

void SlotMigrator::Cancel() { cancel_.store(true, std::memory_order_release); }

void SlotMigrator::Stop() {
  stopped_.store(true, std::memory_order_release);
  Cancel();
  std::thread t;
  {
    std::lock_guard lock(job_mu_);
    if (transport_) transport_->Shutdown();
    t = std::move(thread_);
  }
  if (!t.joinable()) return;
  if (t.get_id() == std::this_thread::get_id()) {
    t.detach();  // Stop from the job's own `done`: a joinable std::thread must never be destroyed
  } else {
    t.join();
  }
}

bool SlotMigrator::WaitIdle(std::chrono::milliseconds timeout) {
  std::unique_lock lock(job_mu_);
  return idle_cv_.wait_for(lock, timeout, [this] { return !running(); });
}

void SlotMigrator::Run(const MigrateJob& job, const MigrateDone& done) {
  next_seq_.store(service_.deps().log.last() + 1, std::memory_order_release);  // pinned before the snapshot
  const Status result = Publish(job, Execute(job));
  next_seq_.store(repl::kNoReplicaNeeds, std::memory_order_release);
  std::unique_ptr<MigrationTransport> link;
  {
    std::lock_guard lock(job_mu_);
    link = std::move(transport_);
    running_.store(false, std::memory_order_release);
  }
  link.reset();  // outside job_mu_ (a leaf): closing an in-process link runs the target's close hook
  idle_cv_.notify_all();
  if (done) done(result);
}

Status SlotMigrator::Execute(const MigrateJob& job) {
  if (Status st = Open(job); !st.ok()) return st;
  uint64_t next = 0;
  if (Status st = SendSnapshot(job, &next); !st.ok()) return st;
  if (hooks_.after_snapshot) hooks_.after_snapshot();
  if (Status st = CatchUp(job, &next); !st.ok()) return st;
  return FenceAndFinish(job, next);
}

Status SlotMigrator::Open(const MigrateJob& job) {
  current_ = settings_();
  // Kvrocks parity: a cancel (a FLUSH, a demotion) still connects and starts the import, so the target sees
  // it fail (IMPORT <R> 2) and shows error; only a Stop (shutdown) skips the target.
  auto t = connect_(job.dst_host, job.dst_port, current_.timeout, &stopped_);
  if (!t.ok()) return Stage(cluster::kErrStageConnect, t.status());
  {
    std::lock_guard lock(job_mu_);
    transport_ = std::move(t).value();
  }
  // a Stop that came before the transport was published
  if (stopped_.load(std::memory_order_acquire)) return Err(cluster::kErrMigrationCanceled);
  if (!current_.requirepass.empty()) {
    if (Status st = transport_->Call({"AUTH", current_.requirepass}); !st.ok()) {
      return Stage(cluster::kErrStageAuth, st);
    }
  }
  const std::string range = cluster::FormatRange(job.range);
  if (Status st = transport_->Call({"CLUSTER", "IMPORT", range, "0"}); !st.ok()) {
    return Stage(cluster::kErrStageImportStatus, st);
  }
  import_started_ = true;
  Delay();  // D25: holds the job in its snapshot phase
  return Canceled();
}

Status SlotMigrator::SendSnapshot(const MigrateJob& job, uint64_t* next) {
  auto snap = service_.deps().log.SnapshotAtLast();  // the view holds exactly the commits <= seq (M6b D2)
  if (!snap.ok()) return Stage(cluster::kErrStageExistingData, snap.status());
  *next = snap->seq + 1;
  next_seq_.store(*next, std::memory_order_release);
  repl::ChunkerOptions o;
  o.cfs = {ColumnFamily::kMeta, ColumnFamily::kSubkey,
           ColumnFamily::kZScore};  // meta first: the target remaps
  o.bounds = SlotRangeBounds(job.range.start, job.range.end);
  o.sys_keys = {};
  repl::SnapshotChunker chunker(*snap->view, 0, o);  // destroyed before the view
  for (;;) {
    current_ = settings_();
    chunker.SetMaxBytes(current_.batch_bytes);
    auto chunk = chunker.Next(nullptr);
    if (!chunk.ok()) return Stage(cluster::kErrStageExistingData, chunk.status());
    const std::optional<std::string>& data = *chunk;
    if (!data.has_value()) return Status::Ok();  // the snapshot is released here (R9)
    if (Status st = SendChunk(*data); !st.ok()) return Stage(cluster::kErrStageExistingData, st);
  }
}

// KM §5.1: up to kMaxCatchUpRounds rounds, each shipping the replog up to the watermark it read, until the
// lag is within migrate-sequence-gap.
Status SlotMigrator::CatchUp(const MigrateJob& job, uint64_t* next) {
  for (int round = 0; round < kMaxCatchUpRounds; ++round) {
    const uint64_t last = service_.deps().log.last();
    if (last + 1 - *next <= current_.sequence_gap) return Status::Ok();
    if (Status st = ShipRange(job, next, last); !st.ok()) return Stage(cluster::kErrStageIncremental, st);
  }
  return Status::Ok();
}

Status SlotMigrator::FenceAndFinish(const MigrateJob& job, uint64_t next) {
  if (hooks_.before_fence) hooks_.before_fence();
  if (Status st = Canceled(); !st.ok()) return st;  // never raise a fence for a job already cancelled
  uint64_t fence_seq = kNotRefenced;                // D4: nothing to the range commits after it
  const Status fenced =
      service_.Mutate([&job](cluster::SlotStates& s) { cluster::SetFenced(s, job.range, true); },
                      /*persist=*/true, &fence_seq);
  if (!fenced.ok()) return Stage(cluster::kErrStageLastIncremental, fenced);
  // The range was fenced already (Mutate moved no fence and left fence_seq untouched): its bits went up under
  // an earlier exclusive barrier, so nothing to it committed after that barrier's seq, which last() now
  // covers. The final delta then still ships everything up to the fence.
  if (fence_seq == kNotRefenced) fence_seq = service_.deps().log.last();
  if (hooks_.after_fence) hooks_.after_fence();
  Delay();  // D25: holds the fence
  if (Status st = ShipRange(job, &next, fence_seq); !st.ok())
    return Stage(cluster::kErrStageLastIncremental, st);
  if (Status st = Canceled(); !st.ok()) return st;  // a FLUSH or a demotion before success fails the job
  const std::string range = cluster::FormatRange(job.range);
  if (Status st = transport_->Call({"CLUSTER", "IMPORT", range, "1"}); !st.ok()) {
    return Stage(cluster::kErrStageImportStatus, st);
  }
  return Status::Ok();
}

// Replog batches *next .. last, filtered to the range (D2), as chunks; a flush op cancels the job (D10).
Status SlotMigrator::ShipRange(const MigrateJob& job, uint64_t* next, uint64_t last) {
  repl::ReplogReader reader(service_.deps().storage, *next);
  while (reader.next() <= last) {
    std::vector<encoding::ReplOp> ops;
    bool flush = false;
    Status read = reader.Read(last, kReadBytes, [&](uint64_t, std::string_view payload) {
      auto batch = encoding::DecodeReplogBatch(payload);
      if (!batch.ok()) return batch.status();
      std::vector<encoding::ReplOp> in = FilterBatch(batch->ops, job.range, &flush);
      ops.insert(ops.end(), std::make_move_iterator(in.begin()), std::make_move_iterator(in.end()));
      return Status::Ok();
    });
    if (!read.ok()) return read;
    if (flush) {
      Cancel();
      return Canceled();
    }
    if (Status st = SendOps(ops); !st.ok()) return st;
    *next = reader.next();
    next_seq_.store(*next, std::memory_order_release);
  }
  return Status::Ok();
}

Status SlotMigrator::SendOps(const std::vector<encoding::ReplOp>& ops) {
  current_ = settings_();
  for (const std::string& chunk : EncodeChunks(ops, current_.batch_bytes)) {
    if (Status st = SendChunk(chunk); !st.ok()) return st;
  }
  return Status::Ok();
}

Status SlotMigrator::SendChunk(std::string_view chunk) {
  current_ = settings_();
  if (!limiter_.Acquire(chunk.size() + kFrameOverhead, current_.rate_bytes_per_s, cancel_)) return Canceled();
  if (Status st = Canceled(); !st.ok()) return st;
  return transport_->Call({"CLUSTER", "IMPORTDATA", chunk});
}

void SlotMigrator::Delay() {
  const auto until = std::chrono::steady_clock::now() + current_.delay;
  while (std::chrono::steady_clock::now() < until && !cancel_.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(10ms);
  }
}

// Success: migrated is persisted only now, after the target acked (D8). Failure, or a success that cannot be
// recorded (D24.1): IMPORT <R> 2 best effort (D10), then the fence lowered and the block says fail.
Status SlotMigrator::Publish(const MigrateJob& job, const Status& outcome) {
  Status result = outcome;
  if (result.ok()) {
    result = PublishSuccess(job);
    if (result.ok()) {
      spdlog::info("slot(s) {} migrated to {}", cluster::FormatRange(job.range), job.dst_id);
      return result;
    }
    spdlog::error("slot(s) {} reached {}, but the success could not be recorded: {}",
                  cluster::FormatRange(job.range), job.dst_id, result.ToString());
  }
  spdlog::warn("migration of slot(s) {} to {} failed: {}", cluster::FormatRange(job.range), job.dst_id,
               result.ToString());
  PublishFailure(job);
  return result;
}

// D9 (ruling S5): the success record waits while CLIENT PAUSE holds writes; the target acked already, so this
// only delays the MOVED. A cancel or a demotion meanwhile (D10: a manual failover pauses first) ends the wait
// and fails the job instead; checked again once admitted.
Status SlotMigrator::PublishSuccess(const MigrateJob& job) {
  const auto halted = [this] {
    return stopped_.load(std::memory_order_acquire) || cancel_.load(std::memory_order_acquire) ||
           service_.deps().is_replica();
  };
  const BackgroundWrite admitted(service_, halted);
  if (!admitted.admitted() || halted()) return Err(cluster::kErrMigrationCanceled);
  return service_.Mutate([&job](cluster::SlotStates& s) {
    if (s.job && s.job->range == job.range) s.job->state = cluster::MigrateState::kSuccess;
    s.migrated.push_back({job.range, job.dst_id, job.dst_host, job.dst_port});
  });
}

void SlotMigrator::PublishFailure(const MigrateJob& job) {
  if (import_started_ && transport_) {
    const std::string range = cluster::FormatRange(job.range);
    if (Status st = transport_->Call({"CLUSTER", "IMPORT", range, "2"}); !st.ok()) {
      spdlog::warn("cannot tell {} that the import of slot(s) {} failed: {}", job.dst_id, range,
                   st.ToString());
    }
  }
  // D9: the fail record waits for CLIENT UNPAUSE too; a replica writes no record (D21) and does not wait.
  const auto& is_replica = service_.deps().is_replica;
  const BackgroundWrite admitted(
      service_, [this, &is_replica] { return stopped_.load(std::memory_order_acquire) || is_replica(); });
  if (!admitted.admitted() && !is_replica()) return;  // Stop while paused: as a kill -9 here would (D8)
  if (const Status st = service_.PublishJobFailure(job.range); !st.ok()) {
    spdlog::warn("cannot publish the failed migration of slot(s) {}: {}; the migrate worker retries",
                 cluster::FormatRange(job.range), st.ToString());
  }
}

}  // namespace tideskv::migrate
