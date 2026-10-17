// MigrationService's SETNODES transitions (M7b D7): states are sticky until a push actually moves a range.
#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "migrate/migration_service.h"

namespace tideskv::migrate {
namespace {

using cluster::SlotRange;

// "Grants the range to me": every slot is mine.
bool OwnsAll(const cluster::Topology& t, SlotRange r) {
  const cluster::ClusterNode* me = t.Myself();
  for (uint32_t s = r.start; s <= r.end; ++s) {
    if (me == nullptr || t.OwnerOf(static_cast<uint16_t>(s)) != me) return false;
  }
  return true;
}

// "Range not mine": no slot is mine any more. A partial move changes nothing until a later push completes it.
bool OwnsNone(const cluster::Topology& t, SlotRange r) {
  const cluster::ClusterNode* me = t.Myself();
  for (uint32_t s = r.start; s <= r.end; ++s) {
    if (me != nullptr && t.OwnerOf(static_cast<uint16_t>(s)) == me) return false;
  }
  return true;
}

bool Listed(const std::vector<SlotRange>& ranges, SlotRange r) {
  return std::find(ranges.begin(), ranges.end(), r) != ranges.end();
}

// An import here owns the data of `r` now (ruling S2(a), conservatively): one in start, or one that
// succeeded.
bool OverlapsAnImport(const cluster::SlotStates& s, SlotRange r) {
  if (s.import && s.import->state == cluster::ImportState::kStart && cluster::Overlaps(s.import->range, r)) {
    return true;
  }
  return std::any_of(s.imported.begin(), s.imported.end(),
                     [r](SlotRange i) { return cluster::Overlaps(i, r); });
}

struct Transition {
  bool cancel = false;              // a job in start whose range is no longer all mine (slot_only, D10)
  std::vector<SlotRange> to_clean;  // the cleanup entries this edit added
};

// D7's table as one edit of `next` (under record_mu_): idempotent, so a burst of pushes collapses safely.
Transition Apply(const cluster::Topology& t, cluster::SlotStates& next) {
  Transition out;
  out.cancel = next.job && next.job->state == cluster::MigrateState::kStart && !OwnsAll(t, next.job->range);
  for (const cluster::MigratedRange& m : next.migrated) {
    if (OwnsNone(t, m.range) && !Listed(next.cleanup, m.range) && !OverlapsAnImport(next, m.range)) {
      out.to_clean.push_back(m.range);
    }
  }
  // Persisted before the eraser starts (D7); the migrated entry and its fence stay until it finishes.
  next.cleanup.insert(next.cleanup.end(), out.to_clean.begin(), out.to_clean.end());
  std::erase_if(next.imported, [&t](SlotRange r) { return OwnsAll(t, r); });  // granted: owned now
  return out;
}

}  // namespace

void MigrationService::OnTopology() {
  Post([this] { ApplyTransition(); });
}

void MigrationService::OnDemoted() {
  migrator_->Cancel();       // D10: fails at its next check and still tells the target (IMPORT <R> 2)
  importer_->FailRunning();  // a replica has one committer: no more IMPORTDATA here (posted)
  Post([this] {
    const Status st = Mutate([](cluster::SlotStates& s) { s = cluster::SlotStates{}; }, /*persist=*/false);
    if (!st.ok()) spdlog::warn("cannot clear the migration states after a demotion: {}", st.ToString());
  });
}

// Task 13 seam (CLIENT PAUSE, D9): the Mutate below and the cleanups are background writes.
void MigrationService::ApplyTransition() {
  if (stopping() || deps_.is_replica()) return;  // a demotion is OnDemoted's
  const std::shared_ptr<const cluster::Topology> t = deps_.cluster.Current();
  Transition done;
  const Status st = Mutate([&t, &done](cluster::SlotStates& next) { done = Apply(*t, next); });
  if (done.cancel) migrator_->Cancel();
  if (!st.ok()) {  // nothing was added
    if (stopping()) return;
    spdlog::warn("cannot record a topology transition: {}; retrying", st.ToString());
    WaitBeforeRetry(kFailureRetry);
    OnTopology();
    return;
  }
  for (SlotRange r : done.to_clean) RunCleanup(r);  // inline on the worker: CLUSTERX waits (Writer's note 4)
}

}  // namespace tideskv::migrate
