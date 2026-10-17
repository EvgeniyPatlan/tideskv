#include "migrate/slot_importer.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <utility>

#include "cluster/cluster_errors.h"
#include "cluster/slot_states.h"
#include "migrate/migration_service.h"

namespace tideskv::migrate {
namespace {

Status Err(std::string_view text) { return Status::UserError(std::string(text)); }

Status Mismatch(const cluster::SlotStates& s, cluster::SlotRange got) {
  const std::string have = s.import ? cluster::FormatRange(s.import->range) : std::string("-1");
  return Err(std::string(cluster::kErrImportMismatchPrefix) + have +
             std::string(cluster::kImportMismatchMiddle) + cluster::FormatRange(got));
}

bool CleanupOverlaps(const cluster::SlotStates& s, cluster::SlotRange range) {
  return std::any_of(s.cleanup.begin(), s.cleanup.end(),
                     [range](cluster::SlotRange c) { return cluster::Overlaps(c, range); });
}

// D14 step 2-3, D15: import start; this node's overlapping migrated ranges (whole, fences included, as
// Kvrocks releases its whole forbidden range) and an overlapping finished source block are released.
void BeginImport(cluster::SlotStates& s, cluster::SlotRange range) {
  s.import = cluster::ImportJob{range, cluster::ImportState::kStart};
  for (const cluster::MigratedRange& m : s.migrated) {
    if (cluster::Overlaps(m.range, range)) cluster::SetFenced(s, m.range, false);
  }
  std::erase_if(s.migrated,
                [range](const cluster::MigratedRange& m) { return cluster::Overlaps(m.range, range); });
  if (s.job && s.job->state != cluster::MigrateState::kStart && cluster::Overlaps(s.job->range, range)) {
    s.job.reset();
  }
}

}  // namespace

SlotImporter::SlotImporter(MigrationService& service)
    : service_(service), remap_([this]() { return service_.deps().oids.Next(); }) {}

bool SlotImporter::running() const {
  std::lock_guard lock(mu_);
  return range_.has_value();
}

// Controller rulings (Task 7 review, S2(b)): the worker is settled before the erase, outside mu_ (worker
// tasks take it). The no-op Mutate performs a pending promotion restore, which posts its half-import erase;
// the AwaitIdle then lets that erase and any running cleanup finish, so neither commits after this import's
// own erase or chunks. An overlapping cleanup found under mu_ is waited for once more.
Status SlotImporter::Start(cluster::SlotRange range, uint64_t link) {
  if (Status st = service_.Mutate([](cluster::SlotStates&) {}); !st.ok()) return st;
  const std::chrono::milliseconds wait = service_.deps().import_worker_wait;
  service_.AwaitIdle(wait);
  bool cleanup_pending = false;
  if (Status st = TryStart(range, link, &cleanup_pending); !st.ok() || !cleanup_pending) return st;
  service_.AwaitIdle(wait);
  if (Status st = TryStart(range, link, &cleanup_pending); !st.ok() || !cleanup_pending) return st;
  return Err(cluster::kErrImportCleanupPending);
}

Status SlotImporter::TryStart(cluster::SlotRange range, uint64_t link, bool* cleanup_pending) {
  *cleanup_pending = false;
  std::lock_guard lock(mu_);
  if (range_) {
    if (*range_ != range)
      return Err(std::string(cluster::kErrOneImportPrefix) + cluster::FormatRange(*range_));
    link_ = link;  // Kvrocks: the same range is idempotent
    return Status::Ok();
  }
  if (CleanupOverlaps(*service_.deps().cluster.States(),
                      range)) {  // never erase what the worker still cleans
    *cleanup_pending = true;
    return Status::Ok();
  }
  if (Status st = service_.EraseRange(range, /*background=*/false); !st.ok()) return st;
  Status st = service_.Mutate([range, cleanup_pending](cluster::SlotStates& s) {
    if (CleanupOverlaps(s, range)) {  // S2(b): checked again under record_mu_; nothing changes then
      *cleanup_pending = true;
      return;
    }
    BeginImport(s, range);
  });
  if (!st.ok() || *cleanup_pending) return st;
  range_ = range;
  link_ = link;
  remap_.Clear();
  return Status::Ok();
}

// D2 steps 2-5 for one chunk: each derived expire PUT right after its meta PUT, so a later DEL of the same
// source txn still removes it. DerivedExpire decodes every meta PUT, so a malformed meta (which RemapOp
// passes through unremapped) fails the import instead of landing with a source oid (Task 2 review).
Status SlotImporter::Translate(cluster::SlotRange range, const std::vector<encoding::ReplOp>& in,
                               std::vector<encoding::ReplOp>* out) {
  for (const encoding::ReplOp& op : in) {
    if (Status st = CheckImportOp(op, range); !st.ok()) return st;
    std::optional<encoding::ReplOp> mapped;
    if (Status st = MoveInto(RemapOp(op, remap_), &mapped); !st.ok()) return st;
    if (!mapped) continue;
    std::optional<encoding::ReplOp> expire;
    if (Status st = MoveInto(DerivedExpire(*mapped), &expire); !st.ok()) return st;
    out->push_back(std::move(*mapped));
    if (expire) out->push_back(std::move(*expire));
  }
  return Status::Ok();
}

Status SlotImporter::Data(uint64_t link, std::string_view chunk) {
  std::lock_guard lock(mu_);
  if (!range_ || link != link_) return Err(cluster::kErrImportNotRunning);
  const cluster::SlotRange range = *range_;
  if (service_.deps().is_replica()) {  // demoted: a replica has one committer (its replication link)
    spdlog::warn("import of slot(s) {} failed: this node is a replica now", cluster::FormatRange(range));
    // Kvrocks StopForLinkError: a replica keeps the data; its new master's Restore erases it (D8, C9).
    if (Status f = FailLocked(range, /*erase=*/false, /*background=*/false); !f.ok()) {
      spdlog::warn("cannot fail the import of slot(s) {} cleanly: {}", cluster::FormatRange(range),
                   f.ToString());
    }
    return Err(cluster::kErrSlaveCantImport);
  }
  auto ops = DecodeChunk(chunk);
  Status st = ops.status();
  std::vector<encoding::ReplOp> out;
  if (st.ok()) st = Translate(range, *ops, &out);
  if (st.ok()) st = service_.deps().engine.ApplyImport(out);
  if (st.ok()) return st;
  spdlog::warn("import of slot(s) {} failed: {}", cluster::FormatRange(range), st.ToString());
  const bool replica = service_.deps().is_replica();
  if (Status f = FailLocked(range, /*erase=*/!replica, /*background=*/false); !f.ok()) {
    spdlog::warn("cannot fail the import of slot(s) {} cleanly: {}", cluster::FormatRange(range),
                 f.ToString());
  }
  return st;
}

Status SlotImporter::Finish(cluster::SlotRange range, bool success) {
  std::lock_guard lock(mu_);
  const auto states = service_.deps().cluster.States();
  const std::optional<cluster::ImportJob> import = states->import;
  if (!import || import->range != range) return Mismatch(*states, range);
  const bool live = range_.has_value() && *range_ == range;
  if (!success) return live ? FailLocked(range, /*erase=*/true, /*background=*/false) : Status::Ok();
  if (!live) {
    return import->state == cluster::ImportState::kSuccess ? Status::Ok()
                                                           : Err(cluster::kErrImportNotRunning);
  }
  Status st = service_.Mutate([range](cluster::SlotStates& s) {
    if (s.import) s.import->state = cluster::ImportState::kSuccess;  // the published import, under mu_
    std::erase_if(s.imported, [range](cluster::SlotRange r) { return cluster::Overlaps(r, range); });
    s.imported.push_back(range);
  });
  if (!st.ok()) return st;
  range_.reset();
  link_ = 0;
  remap_.Clear();
  return Status::Ok();
}

Status SlotImporter::FailLocked(cluster::SlotRange range, bool erase, bool background) {
  range_.reset();
  link_ = 0;
  remap_.Clear();
  if (erase) {
    const Status st = background ? service_.EraseWaitingOutBusy(range, /*background=*/true)
                                 : service_.EraseRange(range, /*background=*/false);
    if (!st.ok()) return st;
  }
  // D9 (ruling S5): on the worker the record write waits while CLIENT PAUSE holds writes; a command's path is
  // admitted already, and a replica (or a node demoted during the wait) writes no record (D21), so neither
  // waits.
  std::optional<BackgroundWrite> admitted;
  if (background) admitted.emplace(service_, service_.deps().is_replica);
  if (admitted && !admitted->admitted() && !service_.deps().is_replica()) {
    return Status::Internal("shutting down");
  }
  return service_.Mutate([range](cluster::SlotStates& s) {
    if (s.import && s.import->range == range) s.import->state = cluster::ImportState::kError;
  });
}

void SlotImporter::FailOnWorker(uint64_t link, bool any_link, const char* why) {
  std::lock_guard lock(mu_);
  if (!range_ || (!any_link && link_ != link)) return;  // finished, or another link took the import over
  const cluster::SlotRange range = *range_;
  // Kvrocks StopForLinkError: a replica keeps the data; its new master's Restore erases it (D8, C9).
  const bool replica = service_.deps().is_replica();
  spdlog::warn("import of slot(s) {} failed: {}", cluster::FormatRange(range), why);
  if (Status st = FailLocked(range, /*erase=*/!replica, /*background=*/true); !st.ok()) {
    spdlog::warn("cannot fail the import of slot(s) {} cleanly: {}", cluster::FormatRange(range),
                 st.ToString());
  }
}

void SlotImporter::LinkClosed(uint64_t link) {
  service_.Post([this, link] { FailOnWorker(link, /*any_link=*/false, "the import link was lost"); });
}

void SlotImporter::FailRunning(const char* why) {
  service_.Post([this, why] { FailOnWorker(0, /*any_link=*/true, why); });
}

}  // namespace tideskv::migrate
