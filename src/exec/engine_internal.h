#pragma once
#include <algorithm>
#include <cassert>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "encoding/key_codec.h"
#include "encoding/replog_codec.h"
#include "exec/engine.h"
#include "exec/recording_txn.h"

namespace tideskv::engine_internal {

inline thread_local Engine::Batch* t_batch = nullptr;  // the active batch of this thread (G1)
inline thread_local bool t_wait_through_busy = false;  // an Engine::WaitThroughBusyScope is alive
inline thread_local bool t_loading_read = false;       // an Engine::LoadingReadScope is alive (CF6)

#ifndef NDEBUG
inline thread_local bool t_in_engine = false;

// A nested Engine call from inside a fn would self-deadlock on the non-reentrant Gate.
class ReentryCheck {
 public:
  ReentryCheck() {
    assert(!t_in_engine && "an Engine fn called back into the Engine");
    t_in_engine = true;
  }
  ~ReentryCheck() { t_in_engine = false; }
  ReentryCheck(const ReentryCheck&) = delete;
  ReentryCheck& operator=(const ReentryCheck&) = delete;
};
#else
struct ReentryCheck {};
#endif

// What one write reports once it commits (G2).
struct WriteScope {
  uint8_t db = 0;
  const std::vector<std::string_view>* keys = nullptr;  // Write: the locked keys
  std::vector<std::string>* also_modified = nullptr;    // Write: filled by fn
  const std::vector<uint8_t>* flushed_dbs = nullptr;    // WriteExclusive
};

// The keys one write changed: the flushed keys, then (if it wrote anything) every key whose meta
// record it touched and the single key of a one-key write, then also_modified. Sorted, unique.
inline std::vector<DbKey> ModifiedBy(bool wrote, std::span<const DbKey> meta_keys, const WriteScope& scope,
                                     std::vector<DbKey> out) {
  if (wrote) {
    out.insert(out.end(), meta_keys.begin(), meta_keys.end());
    if (scope.keys != nullptr && scope.keys->size() == 1)
      out.push_back({scope.db, std::string((*scope.keys)[0])});
  }
  if (scope.also_modified != nullptr) {
    for (const std::string& k : *scope.also_modified) out.push_back({scope.db, k});
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

// True iff `op` deletes the whole meta range of `db` (a FLUSHDB or FLUSHALL on the master).
inline bool DeletesDb(const encoding::ReplOp& op, uint8_t db) {
  if (op.cf != ColumnFamily::kMeta) return false;
  const std::string lo = encoding::DbPrefix(db);
  if (op.type == encoding::ReplOpType::kDeletePrefix) return lo.starts_with(op.key);
  if (op.type != encoding::ReplOpType::kDeleteRange) return false;
  return op.key <= lo && (op.value.empty() || encoding::DbPrefix(static_cast<uint8_t>(db + 1)) <= op.value);
}

// Replica apply (M6a D11): the dbs a master batch flushes, so their watched keys are reported as a
// local flush's are (Redis touchAllWatchedKeysInDb). Sorted, unique.
inline std::vector<uint8_t> FlushedDbs(const std::vector<encoding::ReplOp>& ops) {
  std::vector<uint8_t> out;
  for (uint8_t db = 0; db < encoding::kNumDatabases; ++db) {
    if (std::any_of(ops.begin(), ops.end(), [db](const encoding::ReplOp& op) { return DeletesDb(op, db); }))
      out.push_back(db);
  }
  return out;
}

}  // namespace tideskv::engine_internal
