#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/status.h"
#include "storage/storage.h"

// String type logic (spec §4.4) over one ReadView/WriteTxn. No networking, no threads.
namespace tideskv {

struct SetOptions {
  enum class Condition : uint8_t { kAlways, kIfMissing /* NX */, kIfExists /* XX */ };
  Condition condition = Condition::kAlways;
  bool get = false;       // report the old value (SET ... GET, GETSET)
  bool keep_ttl = false;  // KEEPTTL
  int64_t expire_ms = 0;  // absolute deadline; 0 = none; ignored with keep_ttl
};

struct SetResult {
  bool written = false;
  std::optional<std::string> old_value;  // filled only when SetOptions::get
};

using KeyValue = std::pair<std::string_view, std::string_view>;

// nullopt when missing or expired; UserError(WRONGTYPE) for non-strings.
Result<std::optional<std::string>> StringGet(ReadView& view, uint8_t db, std::string_view key,
                                             int64_t now_ms);
Status StringSet(WriteTxn& txn, uint8_t db, std::string_view key, std::string_view value,
                 const SetOptions& opts, int64_t now_ms, SetResult* out);
Result<std::optional<std::string>> StringGetDel(WriteTxn& txn, uint8_t db, std::string_view key,
                                                int64_t now_ms);
// MSET (only_if_none_exist = false) and MSETNX. Returns whether anything was written.
Result<bool> StringMultiSet(WriteTxn& txn, uint8_t db, const std::vector<KeyValue>& pairs,
                            bool only_if_none_exist, int64_t now_ms);
// MGET: nullopt for missing, expired and non-string keys.
Result<std::vector<std::optional<std::string>>> StringMultiGet(ReadView& view, uint8_t db,
                                                               const std::vector<std::string_view>& keys,
                                                               int64_t now_ms);

struct GetExChange {
  enum class Kind : uint8_t { kNone, kSetExpire, kPersist };
  Kind kind = Kind::kNone;
  int64_t expire_ms = 0;  // absolute, for kSetExpire
};

// Computes the GETEX change. Called only once the key is known to be a live string, so
// that Redis's check order (lookup/type before expire value) is preserved.
using GetExResolver = std::function<Result<GetExChange>()>;

// GETEX: returns the value (nullopt if missing) and applies the resolved change.
Result<std::optional<std::string>> StringGetEx(WriteTxn& txn, uint8_t db, std::string_view key,
                                               const GetExResolver& resolve, int64_t now_ms);
// APPEND: returns the new length. Keeps an existing TTL.
Result<int64_t> StringAppend(WriteTxn& txn, uint8_t db, std::string_view key, std::string_view tail,
                             int64_t now_ms);
// SETRANGE (offset >= 0, checked by the caller): returns the new length. Keeps an existing TTL.
Result<int64_t> StringSetRange(WriteTxn& txn, uint8_t db, std::string_view key, int64_t offset,
                               std::string_view value, int64_t now_ms);
// GETRANGE/SUBSTR index arithmetic (Redis 7.2 getrangeCommand).
std::string_view RedisRange(std::string_view s, int64_t start, int64_t end);
// INCR/DECR/INCRBY/DECRBY: returns the new value. Keeps an existing TTL.
Result<int64_t> StringIncrBy(WriteTxn& txn, uint8_t db, std::string_view key, int64_t delta, int64_t now_ms);
// INCRBYFLOAT: returns the new value as stored (FormatLongDouble). Keeps an existing TTL.
Result<std::string> StringIncrByFloat(WriteTxn& txn, uint8_t db, std::string_view key, long double delta,
                                      int64_t now_ms);

}  // namespace tideskv
