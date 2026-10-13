#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "storage/storage.h"
#include "types/collection.h"
#include "types/oid_allocator.h"

// List type logic (spec §4.2, M4 plan F1) on the collection layer. The element at logical
// position i (0 = leftmost) is the subkey field EncodeListIndex(meta.head + i); meta.size ==
// tail - head + 1. Every function answers UserError(kErrWrongType) for a live key of another type.
namespace tideskv {

enum class ListEnd : uint8_t { kLeft, kRight };

// LPUSH/RPUSH (only_if_exists = false) and LPUSHX/RPUSHX (true): pushes `values` in order, each at
// `end`, and returns the new length; 0 for a missing key when only_if_exists.
Result<int64_t> ListPush(WriteTxn& txn, OidAllocator& oids, uint8_t db, std::string_view key, ListEnd end,
                         const std::vector<std::string_view>& values, bool only_if_exists, int64_t now_ms);
// LPOP/RPOP: removes up to `count` elements from `end` and returns them in pop order; nullopt for a
// missing key. The key goes with its last element.
Result<std::optional<std::vector<std::string>>> ListPop(WriteTxn& txn, uint8_t db, std::string_view key,
                                                        ListEnd end, uint64_t count, int64_t now_ms);
Result<int64_t> ListLen(ReadView& view, uint8_t db, std::string_view key, int64_t now_ms);
// LINDEX. Redis looks the key up before parsing the index: nullopt for a missing key (whatever the
// index), kErrNotInteger for a bad index, nullopt for an index outside [-len, len).
Result<std::optional<std::string>> ListIndex(ReadView& view, uint8_t db, std::string_view key,
                                             std::string_view index_arg, int64_t now_ms);
// LSET, key first then index: kErrNoSuchKey, kErrNotInteger or kErrIndexOutOfRange.
Status ListSet(WriteTxn& txn, uint8_t db, std::string_view key, std::string_view index_arg,
               std::string_view value, int64_t now_ms);
// LRANGE with Redis's index rules (NormalizeListRange); empty for a missing key.
Result<std::vector<std::string>> ListRange(ReadView& view, uint8_t db, std::string_view key, int64_t start,
                                           int64_t stop, int64_t now_ms);

// Building blocks shared by list_edit and list_move.
std::string ListElementKey(const Collection& c, uint64_t position);  // position 0 = leftmost
// Corruption if the element is missing (meta.size promised it).
Result<std::string> ListElementAt(ReadView& view, const Collection& c, uint64_t position);
// Removes and returns the element at `end` of the non-empty list c, updating c.obj.meta (the caller
// saves the collection).
Result<std::string> PopElement(WriteTxn& txn, Collection& c, ListEnd end);
// Writes value at `end` of c, updating c.obj.meta (the caller saves the collection).
Status PushElement(WriteTxn& txn, Collection& c, ListEnd end, std::string_view value);
// Appends the values at positions [from, to] (inclusive, from <= to < size) to *out, left to right.
Status ReadListRange(ReadView& view, const Collection& c, uint64_t from, uint64_t to,
                     std::vector<std::string>* out);
// Redis t_list.c range rule: negative indexes count from the right, start clamps to 0, stop to
// len-1; false when the range is empty.
bool NormalizeListRange(int64_t start, int64_t stop, uint64_t len, uint64_t* from, uint64_t* to);

}  // namespace tideskv
