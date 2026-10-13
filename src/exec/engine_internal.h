#pragma once
#include <algorithm>
#include <cassert>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "exec/engine.h"
#include "exec/recording_txn.h"

namespace tideskv::engine_internal {

inline thread_local Engine::Batch* t_batch = nullptr;  // the active batch of this thread (G1)

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
    if (scope.keys != nullptr && scope.keys->size() == 1) out.push_back({scope.db, std::string((*scope.keys)[0])});
  }
  if (scope.also_modified != nullptr) {
    for (const std::string& k : *scope.also_modified) out.push_back({scope.db, k});
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

}  // namespace tideskv::engine_internal
