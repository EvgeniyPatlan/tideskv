#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "storage/storage.h"
#include "types/list_type.h"
#include "types/oid_allocator.h"

// Multi-key list operations (redis72-list.md §2.11-2.12, §3.1).
namespace tideskv {

// LMOVE/RPOPLPUSH: the moved element, or nullopt when src is missing. src is checked before dst; a dst
// of another type is WRONGTYPE with src untouched; a missing dst is created; src == dst rotates.
Result<std::optional<std::string>> ListMove(WriteTxn& txn, OidAllocator& oids, uint8_t db,
                                            std::string_view src, std::string_view dst, ListEnd from,
                                            ListEnd to, int64_t now_ms);

struct PoppedFrom {
  std::string key;  // as given in the arguments
  std::vector<std::string> values;
};

// LMPOP / BLPOP / BRPOP / BLMPOP core: keys in order; a missing key is skipped, a live non-list is
// WRONGTYPE (later keys are not examined), the first list is popped (up to count from `end`).
// nullopt when no key holds a list.
Result<std::optional<PoppedFrom>> ListPopFirst(WriteTxn& txn, uint8_t db,
                                               const std::vector<std::string_view>& keys, ListEnd end,
                                               uint64_t count, int64_t now_ms);

}  // namespace tideskv
