#pragma once
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "storage/storage.h"

// LINSERT, LREM, LTRIM and LPOS (M4 plan F1): O(n) operations that shift elements on one side,
// in batches with the iterator closed before each write. WRONGTYPE for a live non-list key.
namespace tideskv {

enum class InsertWhere : uint8_t { kBefore, kAfter };

// LINSERT: the new length; 0 for a missing key; -1 if no element equals `pivot`. The side with fewer
// elements is shifted.
Result<int64_t> ListInsert(WriteTxn& txn, uint8_t db, std::string_view key, InsertWhere where,
                           std::string_view pivot, std::string_view value, int64_t now_ms);
// LREM: count > 0 removes up to count matches from the left, count < 0 up to |count| from the right,
// 0 all of them. Returns the number removed; the key goes with its last element.
Result<int64_t> ListRemove(WriteTxn& txn, uint8_t db, std::string_view key, int64_t count,
                           std::string_view value, int64_t now_ms);
// LTRIM: keeps [start, stop] (Redis index rules); an empty range deletes the key. A missing key is OK.
Status ListTrim(WriteTxn& txn, uint8_t db, std::string_view key, int64_t start, int64_t stop, int64_t now_ms);

struct ListPosOptions {
  int64_t rank = 1;                             // != 0; negative scans from the right
  std::optional<int64_t> count = std::nullopt;  // absent: one match; 0: every match
  int64_t maxlen = 0;                           // elements to examine; 0 = all
};
// LPOS: nullopt for a missing key; otherwise the matching positions (counted from the left) in scan
// order, starting at the rank-th match: at most one without COUNT.
Result<std::optional<std::vector<int64_t>>> ListPos(ReadView& view, uint8_t db, std::string_view key,
                                                    std::string_view value, const ListPosOptions& opts,
                                                    int64_t now_ms);

}  // namespace tideskv
