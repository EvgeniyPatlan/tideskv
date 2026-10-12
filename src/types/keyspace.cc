#include "types/keyspace.h"

#include "encoding/key_codec.h"
#include "encoding/meta_codec.h"
#include "types/errors.h"
#include "types/object_store.h"

namespace tideskv {

Result<int64_t> DeleteKeys(WriteTxn& txn, uint8_t db, const std::vector<std::string_view>& keys,
                           int64_t now_ms) {
  int64_t deleted = 0;
  for (std::string_view key : keys) {
    auto obj = LoadForWrite(txn, db, key, now_ms);
    if (!obj.ok()) return obj.status();
    if (!obj->live()) continue;
    if (Status st = DeleteObject(txn, db, key, obj->meta); !st.ok()) return st;
    ++deleted;
  }
  return deleted;
}

Result<int64_t> CountExisting(ReadView& view, uint8_t db, const std::vector<std::string_view>& keys,
                              int64_t now_ms) {
  int64_t n = 0;
  for (std::string_view key : keys) {
    auto obj = LoadObject(view, db, key, now_ms);
    if (!obj.ok()) return obj.status();
    if (obj->live()) ++n;
  }
  return n;
}

Result<std::string> TypeOf(ReadView& view, uint8_t db, std::string_view key, int64_t now_ms) {
  auto obj = LoadObject(view, db, key, now_ms);
  if (!obj.ok()) return obj.status();
  return obj->live() ? std::string(encoding::TypeName(obj->meta.type)) : std::string("none");
}

Status FlushDb(WriteTxn& txn, uint8_t db) {
  for (ColumnFamily cf : kKeyspaceCfs) {
    if (Status st = txn.DeletePrefix(cf, encoding::DbPrefix(db)); !st.ok()) return st;
  }
  return Status::Ok();
}

Status FlushAll(WriteTxn& txn) {
  for (ColumnFamily cf : kKeyspaceCfs) {
    if (Status st = txn.DeleteRange(cf, encoding::DbPrefix(0), encoding::DbPrefix(encoding::kNumDatabases));
        !st.ok()) {
      return st;
    }
  }
  return Status::Ok();
}

Result<uint64_t> CountKeys(ReadView& view, uint8_t db, int64_t now_ms) {
  auto it = view.NewIterator(ColumnFamily::kMeta,
                             {encoding::DbPrefix(db), encoding::DbPrefix(static_cast<uint8_t>(db + 1))});
  if (!it.ok()) return it.status();
  uint64_t n = 0;
  Status st = (*it)->SeekToFirst();
  for (; st.ok() && (*it)->Valid(); st = (*it)->Next()) {
    auto meta = encoding::DecodeMetadata((*it)->value());
    if (!meta.ok()) return meta.status();
    if (!meta->meta.IsExpired(now_ms)) ++n;
  }
  if (!st.ok()) return st;
  return n;
}

namespace {

// Redis 7.2 expireGenericCommand condition checks; current == 0 means no deadline (infinite).
bool ConditionHolds(const ExpireCondition& c, int64_t current, int64_t when) {
  if (c.nx && current != 0) return false;
  if (c.xx && current == 0) return false;
  if (c.gt && (current == 0 || when <= current)) return false;
  if (c.lt && current != 0 && when >= current) return false;
  return true;
}

}  // namespace

Result<int64_t> SetKeyExpire(WriteTxn& txn, uint8_t db, std::string_view key, int64_t when_ms,
                             const ExpireCondition& cond, int64_t now_ms) {
  auto obj = LoadForWrite(txn, db, key, now_ms);
  if (!obj.ok()) return obj.status();
  if (!obj->live() || !ConditionHolds(cond, obj->meta.expire_ms, when_ms)) return int64_t{0};
  const Status st = when_ms <= now_ms ? DeleteObject(txn, db, key, obj->meta)
                                      : SetObjectExpire(txn, db, key, *obj, when_ms);
  if (!st.ok()) return st;
  return int64_t{1};
}

Result<int64_t> KeyTtlMs(ReadView& view, uint8_t db, std::string_view key, bool absolute, int64_t now_ms) {
  auto obj = LoadObject(view, db, key, now_ms);
  if (!obj.ok()) return obj.status();
  if (!obj->live()) return int64_t{-2};
  if (obj->meta.expire_ms == 0) return int64_t{-1};
  const int64_t v = absolute ? obj->meta.expire_ms : obj->meta.expire_ms - now_ms;
  return v < 0 ? int64_t{0} : v;
}

Result<int64_t> PersistKey(WriteTxn& txn, uint8_t db, std::string_view key, int64_t now_ms) {
  auto obj = LoadForWrite(txn, db, key, now_ms);
  if (!obj.ok()) return obj.status();
  if (!obj->live() || obj->meta.expire_ms == 0) return int64_t{0};
  if (Status st = SetObjectExpire(txn, db, key, *obj, 0); !st.ok()) return st;
  return int64_t{1};
}

Result<int64_t> RenameKey(WriteTxn& txn, uint8_t db, std::string_view src, std::string_view dst, bool nx,
                          int64_t now_ms) {
  auto from = LoadForWrite(txn, db, src, now_ms);
  if (!from.ok()) return from.status();
  if (!from->live()) return Status::UserError(std::string(kErrNoSuchKey));
  if (src == dst) return int64_t{nx ? 0 : 1};
  auto to = LoadForWrite(txn, db, dst, now_ms);
  if (!to.ok()) return to.status();
  if (to->live()) {
    if (nx) return int64_t{0};
    if (Status st = DeleteObject(txn, db, dst, to->meta); !st.ok()) return st;
  }
  if (Status st = MoveObject(txn, db, src, dst, *from); !st.ok()) return st;
  return int64_t{1};
}

}  // namespace tideskv
