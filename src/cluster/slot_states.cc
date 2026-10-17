#include "cluster/slot_states.h"

namespace tideskv::cluster {

bool Contains(SlotRange r, uint16_t slot) { return slot >= r.start && slot <= r.end; }

bool Overlaps(SlotRange a, SlotRange b) { return a.start <= b.end && b.start <= a.end; }

std::string FormatRange(SlotRange r) {
  if (r.start == r.end) return std::to_string(r.start);
  return std::to_string(r.start) + "-" + std::to_string(r.end);
}

void SetFenced(SlotStates& s, SlotRange r, bool on) {
  for (uint32_t slot = r.start; slot <= r.end && slot < kSlotCount; ++slot) s.fenced.set(slot, on);
}

std::string_view StateName(MigrateState s) {
  switch (s) {
    case MigrateState::kStart:
      return "start";
    case MigrateState::kSuccess:
      return "success";
    case MigrateState::kFail:
      return "fail";
  }
  return "fail";
}

std::string_view StateName(ImportState s) {
  switch (s) {
    case ImportState::kStart:
      return "start";
    case ImportState::kSuccess:
      return "success";
    case ImportState::kError:
      return "error";
  }
  return "error";
}

const MigratedRange* SlotStates::MigratedOf(uint16_t slot) const {
  for (const MigratedRange& m : migrated) {
    if (Contains(m.range, slot)) return &m;
  }
  return nullptr;
}

bool SlotStates::Importing(uint16_t slot) const {
  return import.has_value() && import->state == ImportState::kStart && Contains(import->range, slot);
}

bool SlotStates::Imported(uint16_t slot) const {
  for (const SlotRange& r : imported) {
    if (Contains(r, slot)) return true;
  }
  return false;
}

}  // namespace tideskv::cluster
