#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "storage/storage.h"

// Keyspace operations shared by every type (spec §12 M2 list).
namespace tideskv {

// CFs whose keys start with the db byte (M2 plan D1). sys and replog are never flushed.
inline constexpr std::array<ColumnFamily, 4> kKeyspaceCfs = {ColumnFamily::kMeta, ColumnFamily::kSubkey,
                                                             ColumnFamily::kZScore, ColumnFamily::kExpire};

// Number of live keys removed (expired ones are cleaned up but not counted).
Result<int64_t> DeleteKeys(WriteTxn& txn, uint8_t db, const std::vector<std::string_view>& keys,
                           int64_t now_ms);
// EXISTS: live keys, duplicates counted.
Result<int64_t> CountExisting(ReadView& view, uint8_t db, const std::vector<std::string_view>& keys,
                              int64_t now_ms);
// "string", "hash", ... or "none".
Result<std::string> TypeOf(ReadView& view, uint8_t db, std::string_view key, int64_t now_ms);
Status FlushDb(WriteTxn& txn, uint8_t db);
Status FlushAll(WriteTxn& txn);
// DBSIZE: live keys of db. O(number of stored keys).
Result<uint64_t> CountKeys(ReadView& view, uint8_t db, int64_t now_ms);

}  // namespace tideskv
