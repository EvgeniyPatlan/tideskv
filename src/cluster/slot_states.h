#pragma once
#include <bitset>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cluster/topology.h"
#include "common/crc16.h"

// The migration states routing reads (M7b D6): immutable once published next to the Topology (readers share
// a std::shared_ptr<const SlotStates>), rebuilt by copy on every change. Plain data: cluster/ stays pure
// (D26).
namespace tideskv::cluster {

enum class MigrateState : uint8_t { kStart, kSuccess, kFail };
enum class ImportState : uint8_t { kStart, kSuccess, kError };

struct SourceJob {  // the last CLUSTERX MIGRATE of this node: the CLUSTER INFO migrating block (D15)
  SlotRange range;
  std::string dst_id;
  std::string dst_host;  // from this node's topology at MIGRATE: where MOVED points after success
  uint16_t dst_port = 0;
  MigrateState state = MigrateState::kStart;
  bool operator==(const SourceJob&) const = default;
};

struct MigratedRange {  // a success not yet moved away by a push: MOVED, fenced (D6, D7)
  SlotRange range;
  std::string dst_id;
  std::string dst_host;
  uint16_t dst_port = 0;
  bool operator==(const MigratedRange&) const = default;
};

struct ImportJob {  // the last CLUSTER IMPORT of this node: the importing block (D15)
  SlotRange range;
  ImportState state = ImportState::kStart;
  bool operator==(const ImportJob&) const = default;
};

struct SlotStates {
  std::optional<SourceJob> job;
  std::bitset<kSlotCount> fenced;  // writes answer TRYAGAIN; the Engine fence mirrors it (D4)
  std::vector<MigratedRange> migrated;
  std::vector<SlotRange> cleanup;  // pushed away; the eraser runs, fenced until done (D7, D9)
  std::optional<ImportJob> import;
  std::vector<SlotRange> imported;  // IMPORT success, not yet granted by a push: served to everyone

  const MigratedRange* MigratedOf(uint16_t slot) const;
  bool Fenced(uint16_t slot) const { return fenced.test(slot); }
  bool Importing(uint16_t slot) const;  // an import in state start covers it
  bool Imported(uint16_t slot) const;
};

bool Contains(SlotRange r, uint16_t slot);
bool Overlaps(SlotRange a, SlotRange b);
std::string FormatRange(SlotRange r);  // "N" when start == end, else "a-b" (Kvrocks SlotRange::String)
void SetFenced(SlotStates& s, SlotRange r, bool on);
std::string_view StateName(MigrateState s);  // start | success | fail
std::string_view StateName(ImportState s);   // start | success | error

}  // namespace tideskv::cluster
