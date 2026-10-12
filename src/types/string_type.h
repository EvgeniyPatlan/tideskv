#pragma once
#include <cstdint>
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

}  // namespace tideskv
