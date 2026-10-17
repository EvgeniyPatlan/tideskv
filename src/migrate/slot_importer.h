#pragma once
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

#include "cluster/topology.h"
#include "common/status.h"
#include "encoding/replog_codec.h"
#include "migrate/migrate_frames.h"

// The target side of one migration (M7b D2, D11, D14): CLUSTER IMPORT start/success/fail and the IMPORTDATA
// chunks of the import link. One import at a time. Lock order: mu_ -> MigrationService::record_mu_ ->
// Engine gate. Never waits for the migrate worker while holding mu_: worker tasks take it.
namespace tideskv::migrate {

class MigrationService;

class SlotImporter {
 public:
  explicit SlotImporter(MigrationService& service);

  // IMPORT <R> 0 from connection `link`: idempotent while R is in start; else waits for the migrate worker
  // (a promotion's half-import erase, an overlapping cleanup: bounded at 10 s), erases R, persists import
  // start, releases this node's overlapping migrated ranges (fences included) and finished source block, and
  // makes `link` the import link. Kvrocks texts for a second range.
  Status Start(cluster::SlotRange range, uint64_t link);
  // IMPORTDATA on `link`: CRC, range and CF checks, oid remap, derived expires, one ApplyImport. Any failure
  // fails the import (erase + error) and is returned. On a replica (demoted meanwhile) nothing is applied:
  // the import fails without an erase and kErrSlaveCantImport is returned.
  Status Data(uint64_t link, std::string_view chunk);
  // IMPORT <R> 1 (success: imported, served) or 2 (fail: erase + error); idempotent for an import that
  // already ended that way.
  Status Finish(cluster::SlotRange range, bool success);
  // ~Connection of `link`: if it is still importing, erase + error on the worker (no erase on a replica).
  void LinkClosed(uint64_t link);
  // FLUSHDB/FLUSHALL on this node (Task 11), a demotion (Task 12): a running import fails, on the worker,
  // with `why` (a string literal) logged. Never blocks. Data refuses chunks on a replica before that runs.
  void FailRunning(const char* why);
  bool running() const;

 private:
  // Start under mu_ (taken here). *cleanup_pending: an overlapping cleanup was found (nothing changed then).
  Status TryStart(cluster::SlotRange range, uint64_t link, bool* cleanup_pending);
  Status Translate(cluster::SlotRange range, const std::vector<encoding::ReplOp>& in,
                   std::vector<encoding::ReplOp>* out);
  // Under mu_: forgets the import, erases its range (unless !erase; BUSY waited out when background),
  // publishes error. An erase failure leaves the persisted start, which the next Restore erases.
  Status FailLocked(cluster::SlotRange range, bool erase, bool background);
  // On the worker: fails the running import if `link` is its link (any import when any_link).
  void FailOnWorker(uint64_t link, bool any_link, const char* why);

  MigrationService& service_;
  mutable std::mutex mu_;
  std::optional<cluster::SlotRange> range_;  // the import in start, guarded by mu_
  uint64_t link_ = 0;                        // its connection id, guarded by mu_
  OidRemap remap_;                           // guarded by mu_; lives until the import ends (spec §8.4)
};

}  // namespace tideskv::migrate
