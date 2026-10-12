#pragma once
#include <cstdint>
#include <string>
#include <string_view>

#include "common/status.h"
#include "encoding/meta_codec.h"
#include "storage/storage.h"

// The object lifecycle shared by every type (spec §4.2-4.5). This is the only code that
// writes the meta and expire CFs, and it keeps the invariant:
//   meta.expire_ms != 0  <=>  an entry ExpireKeyFor(db, expire_ms, key) -> oid exists.
namespace tideskv {

enum class ObjectState : uint8_t { kMissing, kExpired, kLive };

struct StoredObject {
  ObjectState state = ObjectState::kMissing;
  encoding::Metadata meta;
  std::string payload;  // STRING value

  bool live() const { return state == ObjectState::kLive; }
  bool IsString() const { return live() && meta.type == encoding::ValueType::kString; }
};

std::string ExpireValue(uint64_t oid);  // value of an expire-CF entry: big-endian oid (0 for strings)

// Reads (db, key). An object whose deadline is <= now_ms is reported as kExpired.
Result<StoredObject> LoadObject(ReadView& view, uint8_t db, std::string_view key, int64_t now_ms);
// For writers: like LoadObject, but an expired object is deleted in txn (with its expire
// entry and collection data) and reported as kMissing.
Result<StoredObject> LoadForWrite(WriteTxn& txn, uint8_t db, std::string_view key, int64_t now_ms);
// Deletes meta, the expire entry (if any) and the collection data of the object.
Status DeleteObject(WriteTxn& txn, uint8_t db, std::string_view key, const encoding::Metadata& meta);
// Replaces `old` (from LoadForWrite) with a string. expire_ms is absolute; 0 = no expiry.
Status WriteString(WriteTxn& txn, uint8_t db, std::string_view key, std::string_view value, int64_t expire_ms,
                   const StoredObject& old);
// Changes the deadline of a live object (0 = persist) and moves its expire entry.
Status SetObjectExpire(WriteTxn& txn, uint8_t db, std::string_view key, const StoredObject& obj,
                       int64_t expire_ms);
// For the expire reaper only (documented exception to "writers call LoadForWrite"): removes the
// index entry (db, expire_ms, key) without touching meta.
Status DeleteStaleExpireEntry(WriteTxn& txn, uint8_t db, std::string_view key, uint64_t expire_ms);

}  // namespace tideskv
