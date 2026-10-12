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

// EXPIRE options; every flag given is applied, as in Redis (XX LT is not the same as LT).
struct ExpireCondition {
  bool nx = false, xx = false, gt = false, lt = false;
};

// EXPIRE family. when_ms <= now_ms deletes the key. Returns 1 if the deadline was set or
// the key deleted, 0 if the key is missing or the condition failed.
Result<int64_t> SetKeyExpire(WriteTxn& txn, uint8_t db, std::string_view key, int64_t when_ms,
                             const ExpireCondition& cond, int64_t now_ms);
// TTL family: -2 missing, -1 no deadline, otherwise remaining ms (>= 0) or the absolute deadline.
Result<int64_t> KeyTtlMs(ReadView& view, uint8_t db, std::string_view key, bool absolute, int64_t now_ms);
// PERSIST: 1 if a deadline was removed.
Result<int64_t> PersistKey(WriteTxn& txn, uint8_t db, std::string_view key, int64_t now_ms);

// RENAME / RENAMENX: 1 renamed, 0 not renamed (nx with an existing dst). UserError(kErrNoSuchKey)
// if src is absent; a collection moving to another slot is O(its size).
Result<int64_t> RenameKey(WriteTxn& txn, uint8_t db, std::string_view src, std::string_view dst, bool nx,
                          int64_t now_ms);

}  // namespace tideskv
