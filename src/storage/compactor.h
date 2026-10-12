#pragma once
#include <string_view>

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
  // Synchronous. Empty lo/hi = unbounded on that side; Busy when a compaction holds the CF.
  virtual Status CompactRange(ColumnFamily cf, std::string_view lo, std::string_view hi) = 0;
};

}  // namespace tideskv
