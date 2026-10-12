#pragma once
#include "common/status.h"
#include "storage/column_family.h"

namespace tideskv {

// Storage maintenance for the background CompactionScheduler (M3 plan E6). TidesStorage
// implements it; tests use a fake. Kept off Storage: only the App needs it.
class Compactor {
 public:
  virtual ~Compactor() = default;
  // Flushes memtables, so range tombstones reach sstables where compaction can drop them.
  virtual Status Checkpoint() = 0;
  // Synchronous compaction of the whole CF; Busy when a compaction already holds it. Whole-CF on
  // purpose: an engine range compaction cannot reclaim a FLUSHDB (see TidesStorage::Compact).
  virtual Status Compact(ColumnFamily cf) = 0;
};

}  // namespace tideskv
