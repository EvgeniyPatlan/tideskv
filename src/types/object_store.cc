#include "types/object_store.h"

#include "common/coding.h"
#include "common/crc16.h"
#include "encoding/key_codec.h"
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
  const uint16_t slot = KeySlot(key);
  // M3 revisits this per the Task 3 spike decision (R1: delete_prefix tombstone cost).
  if (Status st = txn.DeletePrefix(ColumnFamily::kSubkey, encoding::SubkeyPrefix(db, slot, meta.oid));
      !st.ok()) {
    return st;
  }
  if (meta.type != ValueType::kZSet) return Status::Ok();
  return txn.DeletePrefix(ColumnFamily::kZScore, encoding::ZScorePrefix(db, slot, meta.oid));
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

Status CheckMovable(const Metadata& meta, std::string_view src, std::string_view dst) {
  if (meta.IsCollection() && KeySlot(src) != KeySlot(dst)) {
    return Status::UserError(std::string(kErrRenameCrossSlot));
  }
  return Status::Ok();
}

Status MoveObject(WriteTxn& txn, uint8_t db, std::string_view src, std::string_view dst,
                  const StoredObject& obj) {
  if (Status st = CheckMovable(obj.meta, src, dst); !st.ok()) return st;
  if (Status st = txn.Delete(ColumnFamily::kMeta, encoding::MetaKeyFor(db, src)); !st.ok()) return st;
  if (Status st = DeleteExpireEntry(txn, db, src, obj.meta); !st.ok()) return st;
  if (Status st = PutMeta(txn, db, dst, obj.meta, obj.payload); !st.ok()) return st;
  return PutExpireEntry(txn, db, dst, obj.meta);
}

}  // namespace tideskv
