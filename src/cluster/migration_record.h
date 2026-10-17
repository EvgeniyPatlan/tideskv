#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cluster/slot_states.h"
#include "common/status.h"
#include "encoding/replog_codec.h"
#include "storage/storage.h"

// sys/migration (M7b D8): the outcomes a restarted or promoted node resumes. Replicated (migrate/ writes it
// through ReplLog::CommitStandalone, like sys/oid_hwm) and streamed by full sync. Integers big-endian:
//   [format u8 = 1]
//   [job present u8 {from u16, to u16, id len u8 + id, host len u16 + host, port u16, state u8}]
//   [migrated count u16 {from, to, id, host, port}...] [cleanup count u16 {from, to}...]
//   [import present u8 {from, to, state u8}] [imported count u16 {from, to}...]
// A running `start` job is never persisted (decoding one is Corruption).
namespace tideskv::cluster {

inline constexpr std::string_view kSysMigration = "migration";
inline constexpr uint8_t kMigrationRecordFormat = 1;

struct MigrationRecord {
  std::optional<SourceJob> job;  // success | fail
  std::vector<MigratedRange> migrated;
  std::vector<SlotRange> cleanup;
  std::optional<ImportJob> import;
  std::vector<SlotRange> imported;
  bool operator==(const MigrationRecord&) const = default;
};

std::string EncodeMigrationRecord(const MigrationRecord& r);
Result<MigrationRecord> DecodeMigrationRecord(std::string_view raw);           // Corruption on any defect
Result<std::optional<MigrationRecord>> LoadMigrationRecord(Storage& storage);  // nullopt when absent
encoding::ReplOp MigrationRecordPut(const MigrationRecord& r);                 // PUT sys/migration
MigrationRecord RecordOf(const SlotStates& s);                                 // the persisted part
SlotStates StatesFromRecord(const MigrationRecord& r);  // boot/promotion: fenced = migrated + cleanup

}  // namespace tideskv::cluster
