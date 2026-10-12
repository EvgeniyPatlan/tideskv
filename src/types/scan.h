#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "encoding/meta_codec.h"
#include "storage/storage.h"

// Keyspace iteration (SCAN/KEYS/RANDOMKEY) over the db's contiguous meta range (D1).
namespace tideskv {

struct ScanFilter {
  std::string_view pattern;                 // empty = everything
  std::optional<encoding::ValueType> type;  // nullopt = any type
  bool match_nothing = false;               // unknown TYPE name: Redis 7.2 returns no keys
};

struct ScanPage {
  std::vector<std::string> keys;
  std::string next_position;  // last examined meta key; empty = iteration finished
};

std::optional<encoding::ValueType> TypeFromName(std::string_view name);
// Examines up to `count` meta entries of db strictly after `position` (empty = from the start).
Status ScanKeys(ReadView& view, uint8_t db, std::string_view position, size_t count, const ScanFilter& filter,
                int64_t now_ms, ScanPage* out);
Status MatchingKeys(ReadView& view, uint8_t db, std::string_view pattern, int64_t now_ms,
                    std::vector<std::string>* out);
// A live key near a random slot, wrapping around; nullopt if db has no live key.
Result<std::optional<std::string>> RandomKey(ReadView& view, uint8_t db, int64_t now_ms, uint64_t random);

}  // namespace tideskv
