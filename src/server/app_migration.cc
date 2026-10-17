// App's slot-migration wiring (M7b), kept out of app.cc, which is near the size limit.
#include <memory>
#include <utility>

#include "server/app.h"

namespace tideskv {

Status App::CreateMigration() {
  if (!cluster_) return Status::Ok();
  migrate::MigrationDeps deps{.engine = *engine_,
                              .log = *repl_log_,
                              .storage = *storage_,
                              .oids = *oids_,
                              .cluster = *cluster_,
                              .is_replica = [this] { return replication_->IsReplica(); },
                              .begin_background_write = {},
                              .end_background_write = {},
                              .make_durable = [this] { return storage_->Checkpoint(); }};
  migration_ = std::make_unique<migrate::MigrationService>(std::move(deps));
  if (StartsAsReplica()) return Status::Ok();  // a replica holds the record passively (D21)
  return migration_->Restore();                // D8: a damaged record stops the server
}

}  // namespace tideskv
