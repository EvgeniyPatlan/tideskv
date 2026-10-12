#include "types/keyspace.h"

#include "encoding/key_codec.h"
#include "encoding/meta_codec.h"
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

}  // namespace tideskv
