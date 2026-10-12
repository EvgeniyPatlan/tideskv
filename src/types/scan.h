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
#include "types/scan_position.h"

// Keyspace iteration (SCAN/KEYS/RANDOMKEY) over the db's contiguous meta range (D1).
namespace tideskv {

struct ScanFilter {
  std::optional<std::string_view> pattern;  // nullopt = everything; Redis: set unless exactly "*"
  std::optional<encoding::ValueType> type;  // nullopt = any type
  bool match_nothing = false;               // unknown TYPE name: Redis 7.2 returns no keys
};

struct ScanPage {
  std::vector<std::string> keys;
  std::string next_position;  // last examined meta key; empty = iteration finished
};

// Positions `it` at the first entry strictly after `pos` (empty key = the first entry); a
// truncated position steps past the key matching its size and digest (see CursorStore).
Status SeekPast(Iterator& it, const ScanPosition& pos);

std::optional<encoding::ValueType> TypeFromName(std::string_view name);
// Examines up to `count` meta entries of db strictly after `position` (empty key = from the
// start). For a truncated position it first walks the keys starting with the saved prefix and
// starts after the one matching full_size and digest, or at the prefix if none does; those
// walked entries are neither counted nor returned.
Status ScanKeys(ReadView& view, uint8_t db, const ScanPosition& position, size_t count,
                const ScanFilter& filter, int64_t now_ms, ScanPage* out);
Status MatchingKeys(ReadView& view, uint8_t db, std::string_view pattern, int64_t now_ms,
                    std::vector<std::string>* out);
// A live key near a random slot, wrapping around; nullopt if db has no live key.
Result<std::optional<std::string>> RandomKey(ReadView& view, uint8_t db, int64_t now_ms, uint64_t random);

struct ElementPage {
  std::vector<std::string> items;  // fields, or field/value pairs flattened when with_values
  std::string next_position;       // last examined subkey; empty = iteration finished
};

// HSCAN/SSCAN: examines up to `count` elements of the collection whose subkey prefix is
// `prefix`, strictly after `position` (empty key = from the first). Fields not matching
// `pattern` are examined (counted) but not returned.
Status ScanElements(ReadView& view, std::string_view prefix, const ScanPosition& position, size_t count,
                    std::optional<std::string_view> pattern, bool with_values, ElementPage* out);

}  // namespace tideskv
