// App's slot-migration wiring (M7b), kept out of app.cc, which is near the size limit.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "migrate/migration_transport.h"
#include "server/app.h"

namespace tideskv {
namespace {

migrate::MigrateSettings SettingsOf(const ConfigValues& v) {  // D17; requirepass: KM §5
  migrate::MigrateSettings s;
  s.sequence_gap = static_cast<uint64_t>(v.migrate_sequence_gap);
  s.batch_bytes = static_cast<size_t>(v.migrate_batch_size_kb) * 1024;
  s.rate_bytes_per_s = static_cast<uint64_t>(v.migrate_batch_rate_limit_mb) << 20;
  s.timeout = std::chrono::seconds(std::max<int64_t>(v.repl_timeout, 1));
  s.requirepass = v.requirepass;
  s.delay = std::chrono::milliseconds(v.migrate_delay_ms);
  return s;
}

}  // namespace

Status App::CreateMigration() {
  if (!cluster_) return Status::Ok();
  migrate::MigrationDeps deps{.engine = *engine_,
                              .log = *repl_log_,
                              .storage = *storage_,
                              .oids = *oids_,
                              .cluster = *cluster_,
                              .is_replica = [this] { return replication_->IsReplica(); },
                              .begin_background_write = [this] { return pause_.BeginBackgroundWrite(); },
                              .end_background_write = [this] { pause_.EndWrite(); },
                              .make_durable = [this] { return storage_->Checkpoint(); },
                              .connect = migrate::ConnectResp,
                              .settings = [this] { return SettingsOf(config_->Snapshot()); }};
  migration_ = std::make_unique<migrate::MigrationService>(std::move(deps));
  if (StartsAsReplica()) return Status::Ok();  // a replica holds the record passively (D21)
  return migration_->Restore();                // D8: a damaged record stops the server
}

}  // namespace tideskv
