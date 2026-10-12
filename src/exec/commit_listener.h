#pragma once
#include <string>
#include <vector>

#include "storage/column_family.h"

namespace tideskv {

// A DeletePrefix (is_prefix: lo = the prefix) or DeleteRange [lo, hi) a transaction committed.
struct RangeDelete {
  ColumnFamily cf = ColumnFamily::kMeta;
  std::string lo;
  std::string hi;  // DeleteRange only; empty = to the end
  bool is_prefix = true;
};

// Told about the range deletes of each committed write (Engine, M3 plan E6). Called on the
// committing thread while its key stripes and the gate are still held: implementations must
// only record or enqueue, never block on storage.
class CommitListener {
 public:
  virtual ~CommitListener() = default;
  virtual void OnCommitted(const std::vector<RangeDelete>& deletes) = 0;
};

}  // namespace tideskv
