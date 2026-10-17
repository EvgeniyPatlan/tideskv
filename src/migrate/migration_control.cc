// MigrationService's command entries (M7b D10, D12, D13, D20).
#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "cluster/cluster_errors.h"
#include "migrate/migration_service.h"

namespace tideskv::migrate {
namespace {

using cluster::SlotRange;

Status Err(std::string_view text) { return Status::UserError(std::string(text)); }

// Kvrocks check 3: the min..max span of the migrated ranges (gocase "Repeat migration ... setslot").
bool OverlapsMigratedSpan(const cluster::SlotStates& s, SlotRange r) {
  if (s.migrated.empty()) return false;
  SlotRange span = s.migrated.front().range;
  for (const cluster::MigratedRange& m : s.migrated) {
    span.start = std::min(span.start, m.range.start);
    span.end = std::max(span.end, m.range.end);
  }
  return cluster::Overlaps(span, r);
}

bool JobInStart(const cluster::SlotStates& s) {
  return s.job.has_value() && s.job->state == cluster::MigrateState::kStart;
}

bool OverlapsFence(const cluster::SlotStates& s, SlotRange r) {  // Kvrocks check 9
  for (uint32_t slot = r.start; slot <= r.end; ++slot) {
    if (s.Fenced(static_cast<uint16_t>(slot))) return true;
  }
  return false;
}

// Kvrocks Cluster::MigrateSlotRange checks 1-7 (cluster.cc:309-346), in order.
Status CheckMigrate(const cluster::Topology& t, const cluster::SlotStates& s, SlotRange r,
                    std::string_view dst_id, bool replica, MigrateJob* job) {
  const cluster::ClusterNode* dst = t.initialized() ? t.Find(dst_id) : nullptr;
  if (dst == nullptr) return Err(cluster::kErrNoDestination);
  if (r.start > r.end || r.end >= kSlotCount) return Err(cluster::kErrSlotRangeInvalid);
  if (OverlapsMigratedSpan(s, r)) return Err(cluster::kErrSlotMigrated);
  const cluster::ClusterNode* me = t.Myself();
  for (uint32_t slot = r.start; slot <= r.end; ++slot) {
    if (me == nullptr || t.OwnerOf(static_cast<uint16_t>(slot)) != me) return Err(cluster::kErrNotMySlot);
  }
  if (me->role != cluster::NodeRole::kMaster || replica) return Err(cluster::kErrSlaveCantMigrate);
  if (dst->role != cluster::NodeRole::kMaster) return Err(cluster::kErrMigrateToSlave);
  if (dst == me) return Err(cluster::kErrMigrateToMyself);
  *job = MigrateJob{r, dst->id, dst->host, dst->port};
  return Status::Ok();
}

}  // namespace

Status MigrationService::Migrate(const std::vector<SlotRange>& ranges, std::string_view dst_id,
                                 MigrateDone done) {
  if (ranges.size() != 1) return Err(cluster::kErrMultipleRanges);
  std::lock_guard lock(migrate_mu_);
  const auto t = deps_.cluster.Current();
  auto s = deps_.cluster.States();
  MigrateJob job;
  if (Status st = CheckMigrate(*t, *s, ranges[0], dst_id, deps_.is_replica(), &job); !st.ok()) return st;
  if (migrator_->running() && !JobInStart(*s)) {
    migrator_->WaitIdle(std::chrono::seconds(1));  // the last job published its outcome and is finishing
  }
  if (migrator_->running()) return Err(cluster::kErrAlreadyMigrating);
  if (OverlapsFence(*s, ranges[0])) return Err(cluster::kErrSlotMigrated);
  if (Status st = Mutate([&job](cluster::SlotStates& next) {
        next.job = cluster::SourceJob{job.range, job.dst_id, job.dst_host, job.dst_port,
                                      cluster::MigrateState::kStart};
      });
      !st.ok()) {
    return st;
  }
  Status started = migrator_->Start(std::move(job), std::move(done));
  if (!started.ok()) {  // stopping: no job will publish an outcome, so the start block must not stay
    (void)Mutate([](cluster::SlotStates& next) {
      if (JobInStart(next)) next.job->state = cluster::MigrateState::kFail;
    });
  }
  return started;
}

void MigrationService::OnFlush() {
  migrator_->Cancel();
  importer_->FailRunning("the keyspace was flushed");
}

bool MigrationService::MigrationRunning() const {
  const auto s = deps_.cluster.States();
  return migrator_->running() || JobInStart(*s);
}

bool MigrationService::ImportRunning() const {
  const std::optional<cluster::ImportJob> import = deps_.cluster.States()->import;
  return import.has_value() && import->state == cluster::ImportState::kStart;
}

Status MigrationService::FlushSlots(const std::vector<SlotRange>& ranges) {
  std::lock_guard lock(migrate_mu_);
  if (MigrationRunning()) return Err(cluster::kErrFlushWhileMigrating);
  for (SlotRange r : ranges) {
    if (Status st = EraseRange(r, /*background=*/false); !st.ok()) return st;
  }
  return Status::Ok();
}

Status MigrationService::OnReset() {
  return Mutate([](cluster::SlotStates& s) {
    if (s.job.has_value() && s.job->state == cluster::MigrateState::kSuccess) s.job.reset();
    s.migrated.clear();
    s.cleanup.clear();
    s.imported.clear();
    s.fenced.reset();
  });
}

Status MigrationService::ResetCluster(const std::function<Status()>& reset) {
  std::lock_guard lock(migrate_mu_);
  if (MigrationRunning()) return Err(cluster::kErrResetWhileMigrating);
  if (ImportRunning()) return Err(cluster::kErrResetWhileImporting);
  deps_.engine.HoldAllFenced();
  Status st = reset();
  if (st.ok()) st = OnReset();
  deps_.engine.ReleaseAllFenced();  // the fence becomes the states' set (C11), whoever moved it meanwhile
  return st;
}

}  // namespace tideskv::migrate
