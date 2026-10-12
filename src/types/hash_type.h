#pragma once
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/status.h"
#include "storage/storage.h"
#include "types/oid_allocator.h"

// Hash type logic (spec §4.4) over one ReadView/WriteTxn, on the collection layer.
// Every function answers UserError(kErrWrongType) for a live key of another type.
namespace tideskv {

using FieldValue = std::pair<std::string_view, std::string_view>;

// HSET/HMSET: returns the number of fields that did not exist. Later duplicates win.
Result<int64_t> HashSet(WriteTxn& txn, OidAllocator& oids, uint8_t db, std::string_view key,
                        const std::vector<FieldValue>& pairs, int64_t now_ms);
// HSETNX: true if the field was created.
Result<bool> HashSetNx(WriteTxn& txn, OidAllocator& oids, uint8_t db, std::string_view key,
                       std::string_view field, std::string_view value, int64_t now_ms);
Result<std::optional<std::string>> HashGet(ReadView& view, uint8_t db, std::string_view key,
                                           std::string_view field, int64_t now_ms);
Result<std::vector<std::optional<std::string>>> HashMultiGet(ReadView& view, uint8_t db, std::string_view key,
                                                             const std::vector<std::string_view>& fields,
                                                             int64_t now_ms);
// HDEL: the number of distinct existing fields removed; the key goes with its last field.
Result<int64_t> HashDelete(WriteTxn& txn, uint8_t db, std::string_view key,
                           const std::vector<std::string_view>& fields, int64_t now_ms);
Result<int64_t> HashLen(ReadView& view, uint8_t db, std::string_view key, int64_t now_ms);
Result<int64_t> HashStrlen(ReadView& view, uint8_t db, std::string_view key, std::string_view field,
                           int64_t now_ms);
Result<bool> HashExists(ReadView& view, uint8_t db, std::string_view key, std::string_view field,
                        int64_t now_ms);

enum class HashPart : uint8_t { kFields, kValues, kBoth };

// HKEYS / HVALS / HGETALL in field byte order; kBoth is flattened f1, v1, f2, v2, ...
Result<std::vector<std::string>> HashGetAll(ReadView& view, uint8_t db, std::string_view key, HashPart part,
                                            int64_t now_ms);
// HINCRBY: returns the new value. A missing field counts as 0.
Result<int64_t> HashIncrBy(WriteTxn& txn, OidAllocator& oids, uint8_t db, std::string_view key,
                           std::string_view field, int64_t delta, int64_t now_ms);
// HINCRBYFLOAT (delta finite, checked by the caller): returns the new value as stored (FormatLongDouble).
Result<std::string> HashIncrByFloat(WriteTxn& txn, OidAllocator& oids, uint8_t db, std::string_view key,
                                    std::string_view field, long double delta, int64_t now_ms);

}  // namespace tideskv
