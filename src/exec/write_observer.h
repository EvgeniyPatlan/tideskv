#pragma once
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "storage/storage.h"

namespace tideskv {

// A user key of one database.
struct DbKey {
  uint8_t db = 0;
  std::string key;
  bool operator==(const DbKey&) const = default;
  bool operator<(const DbKey& o) const { return std::tie(db, key) < std::tie(o.db, o.key); }
};

// Told which user keys each committed write changed (M5a plan G2); implemented by WatchRegistry.
// Called on the writer's thread while the Engine still holds the gate (and, for Write, the key
// stripes), so no other write to these keys commits in between. Must never call the Engine.
class WriteObserver {
 public:
  virtual ~WriteObserver() = default;
  // Nothing to tell anyone (nobody watches anything): the Engine skips reporting. One atomic load.
  virtual bool Idle() const = 0;
  // `keys` are sorted and unique. `committed` reads the state just committed, or is null if it
  // could not be opened (then treat every key as existing).
  virtual void OnModified(const std::vector<DbKey>& keys, ReadView* committed) = 0;
  // Before a flush of `db` runs: the watched keys of `db` that exist in `view` (the flushing txn).
  virtual std::vector<std::string> ExistingWatched(uint8_t db, ReadView& view) = 0;
};

}  // namespace tideskv
