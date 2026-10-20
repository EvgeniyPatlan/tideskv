#include "types/object_store.h"

#include <string>
#include <utility>
#include <vector>

#include "common/coding.h"
#include "encoding/key_codec.h"
#include "encoding/key_layout.h"
#include "encoding/object_codec.h"
#include "types/errors.h"

namespace tideskv {
namespace {

using encoding::Metadata;
using encoding::ValueType;

Status PutExpireEntry(WriteTxn& txn, uint8_t db, std::string_view key, const Metadata& m) {
  if (m.expire_ms == 0) return Status::Ok();
  return txn.Put(ColumnFamily::kExpire, encoding::ExpireKeyFor(db, static_cast<uint64_t>(m.expire_ms), key),
                 ExpireValue(m.oid));
}

Status DeleteExpireEntry(WriteTxn& txn, uint8_t db, std::string_view key, const Metadata& m) {
  if (m.expire_ms == 0) return Status::Ok();
  return txn.Delete(ColumnFamily::kExpire,
                    encoding::ExpireKeyFor(db, static_cast<uint64_t>(m.expire_ms), key));
}

Status PutMeta(WriteTxn& txn, uint8_t db, std::string_view key, const Metadata& m, std::string_view payload) {
  return txn.Put(ColumnFamily::kMeta, encoding::MetaKeyFor(db, key), encoding::EncodeMetadata(m, payload));
}

}  // namespace

Status DeleteStaleExpireEntry(WriteTxn& txn, uint8_t db, std::string_view key, uint64_t expire_ms) {
  return txn.Delete(ColumnFamily::kExpire, encoding::ExpireKeyFor(db, expire_ms, key));
}

std::string ExpireValue(uint64_t oid) {
  std::string out;
  coding::PutFixed64BE(&out, oid);
  return out;
}

bool DecodeExpireValue(std::string_view value, uint64_t* oid) {
  return value.size() == 8 && coding::GetFixed64BE(&value, oid);
}

Result<StoredObject> LoadObject(ReadView& view, uint8_t db, std::string_view key, int64_t now_ms) {
  auto raw = view.Get(ColumnFamily::kMeta, encoding::MetaKeyFor(db, key));
  if (!raw.ok()) {
    if (raw.status().IsNotFound()) return StoredObject{};
    return raw.status();
  }
  auto decoded = encoding::DecodeMetadata(*raw);
  if (!decoded.ok()) return decoded.status();
  StoredObject obj;
  obj.meta = decoded->meta;
  obj.payload = std::string(decoded->payload);
  obj.state = obj.meta.IsExpired(now_ms) ? ObjectState::kExpired : ObjectState::kLive;
  return obj;
}

Result<StoredObject> LoadForWrite(WriteTxn& txn, uint8_t db, std::string_view key, int64_t now_ms) {
  auto obj = LoadObject(txn, db, key, now_ms);
  if (!obj.ok() || obj->state != ObjectState::kExpired) return obj;
  if (Status st = DeleteObject(txn, db, key, obj->meta); !st.ok()) return st;
  return StoredObject{};
}

Status DeleteObject(WriteTxn& txn, uint8_t db, std::string_view key, const Metadata& meta) {
  if (Status st = txn.Delete(ColumnFamily::kMeta, encoding::MetaKeyFor(db, key)); !st.ok()) return st;
  if (Status st = DeleteExpireEntry(txn, db, key, meta); !st.ok()) return st;
  if (!meta.IsCollection()) return Status::Ok();
  const uint16_t slot = encoding::LayoutSlot(key);
  // One O(1) prefix tombstone per CF (M2 Task 3 spike, M3 E6); the CompactionScheduler counts
  // these deletes and periodically compacts the subkey/zscore CFs to drop the dead entries.
  if (Status st = txn.DeletePrefix(ColumnFamily::kSubkey, encoding::SubkeyPrefix(db, slot, meta.oid));
      !st.ok()) {
    return st;
  }
  if (meta.type != ValueType::kZSet) return Status::Ok();
  return txn.DeletePrefix(ColumnFamily::kZScore, encoding::ZScorePrefix(db, slot, meta.oid));
}

Status PutObjectMeta(WriteTxn& txn, uint8_t db, std::string_view key, const Metadata& meta) {
  return PutMeta(txn, db, key, meta, {});
}

Status DeleteEmptyObject(WriteTxn& txn, uint8_t db, std::string_view key, const Metadata& meta) {
  if (Status st = txn.Delete(ColumnFamily::kMeta, encoding::MetaKeyFor(db, key)); !st.ok()) return st;
  return DeleteExpireEntry(txn, db, key, meta);
}

Status WriteString(WriteTxn& txn, uint8_t db, std::string_view key, std::string_view value, int64_t expire_ms,
                   const StoredObject& old) {
  if (old.live()) {
    const Status st = old.meta.IsCollection() ? DeleteObject(txn, db, key, old.meta)
                                              : DeleteExpireEntry(txn, db, key, old.meta);
    if (!st.ok()) return st;
  }
  Metadata m;
  m.type = ValueType::kString;
  m.expire_ms = expire_ms;
  if (Status st = PutMeta(txn, db, key, m, value); !st.ok()) return st;
  return PutExpireEntry(txn, db, key, m);
}

Status SetObjectExpire(WriteTxn& txn, uint8_t db, std::string_view key, const StoredObject& obj,
                       int64_t expire_ms) {
  if (Status st = DeleteExpireEntry(txn, db, key, obj.meta); !st.ok()) return st;
  Metadata m = obj.meta;
  m.expire_ms = expire_ms;
  if (Status st = PutMeta(txn, db, key, m, obj.payload); !st.ok()) return st;
  return PutExpireEntry(txn, db, key, m);
}

namespace {

constexpr size_t kRehomeBatch = 1024;  // entries copied per iterator pass

// Reads up to kRehomeBatch entries of cf under `prefix` strictly after `after` (empty = from the
// first). The iterator is closed before the caller writes, so no write lands mid-iteration.
Status ReadBatch(WriteTxn& txn, ColumnFamily cf, const std::string& prefix, const std::string& after,
                 std::vector<std::pair<std::string, std::string>>* out) {
  out->clear();
  auto it = txn.NewIterator(cf, {prefix, encoding::PrefixUpperBound(prefix)});
  if (!it.ok()) return it.status();
  Status st = after.empty() ? (*it)->SeekToFirst() : (*it)->Seek(after);
  if (st.ok() && !after.empty() && (*it)->Valid() && (*it)->key() == after) st = (*it)->Next();
  for (; st.ok() && (*it)->Valid() && out->size() < kRehomeBatch; st = (*it)->Next()) {
    out->emplace_back(std::string((*it)->key()), std::string((*it)->value()));
  }
  return st;
}

// Copies a collection's entries of cf from its old slot's prefix to the new slot's (the oid,
// and so every suffix, is kept), then range-deletes the old prefix. O(size) (M3 plan E5).
Status RehomeEntries(WriteTxn& txn, ColumnFamily cf, uint8_t db, uint16_t from_slot, uint16_t to_slot,
                     uint64_t oid) {
  const std::string from = encoding::SubkeyPrefix(db, from_slot, oid);
  const std::string to = encoding::SubkeyPrefix(db, to_slot, oid);
  std::vector<std::pair<std::string, std::string>> batch;
  std::string last;
  do {
    if (Status st = ReadBatch(txn, cf, from, last, &batch); !st.ok()) return st;
    for (const auto& [key, value] : batch) {
      if (Status st = txn.Put(cf, to + key.substr(from.size()), value); !st.ok()) return st;
    }
    if (!batch.empty()) last = batch.back().first;
  } while (batch.size() == kRehomeBatch);
  return txn.DeletePrefix(cf, from);
}

}  // namespace

Status MoveObject(WriteTxn& txn, uint8_t db, std::string_view src, std::string_view dst,
                  const StoredObject& obj) {
  const uint16_t from_slot = encoding::LayoutSlot(src);
  const uint16_t to_slot = encoding::LayoutSlot(dst);  // both 0 when slotless: the entries never move
  if (obj.meta.IsCollection() && from_slot != to_slot) {
    if (Status st = RehomeEntries(txn, ColumnFamily::kSubkey, db, from_slot, to_slot, obj.meta.oid);
        !st.ok()) {
      return st;
    }
    if (obj.meta.type == ValueType::kZSet) {
      if (Status st = RehomeEntries(txn, ColumnFamily::kZScore, db, from_slot, to_slot, obj.meta.oid);
          !st.ok()) {
        return st;
      }
    }
  }
  if (Status st = txn.Delete(ColumnFamily::kMeta, encoding::MetaKeyFor(db, src)); !st.ok()) return st;
  if (Status st = DeleteExpireEntry(txn, db, src, obj.meta); !st.ok()) return st;
  if (Status st = PutMeta(txn, db, dst, obj.meta, obj.payload); !st.ok()) return st;
  return PutExpireEntry(txn, db, dst, obj.meta);
}

}  // namespace tideskv
