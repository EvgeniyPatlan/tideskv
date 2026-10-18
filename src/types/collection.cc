#include "types/collection.h"

#include <algorithm>
#include <memory>

#include "common/crc16.h"
#include "encoding/key_codec.h"
#include "encoding/object_codec.h"
#include "types/errors.h"
#include "types/sample.h"

namespace tideskv {
namespace {

Result<Collection> Typed(Result<StoredObject> obj, uint8_t db, std::string_view key,
                         encoding::ValueType type) {
  if (!obj.ok()) return obj.status();
  if (obj->live() && obj->meta.type != type) return Status::UserError(std::string(kErrWrongType));
  Collection c;
  c.db = db;
  c.key = std::string(key);
  c.slot = KeySlot(key);
  c.stored = obj->live();
  if (obj->live()) {
    c.obj = std::move(obj).value();
    c.stored_meta = encoding::EncodeMetadata(c.obj.meta);
  }
  return c;
}

Result<std::unique_ptr<Iterator>> ElementIterator(ReadView& view, const Collection& c) {
  std::string lower = c.Prefix();
  std::string upper = encoding::PrefixUpperBound(lower);
  return view.NewIterator(ColumnFamily::kSubkey, {std::move(lower), std::move(upper)});
}

}  // namespace

std::string Collection::Prefix() const { return encoding::SubkeyPrefix(db, slot, obj.meta.oid); }

std::string Collection::SubkeyFor(std::string_view field) const {
  return encoding::EncodeSubkey(db, slot, obj.meta.oid, field);
}

Result<Collection> LoadCollection(ReadView& view, uint8_t db, std::string_view key, encoding::ValueType type,
                                  int64_t now_ms) {
  auto obj = LoadObject(view, db, key, now_ms);
  if (obj.ok() && obj->state == ObjectState::kExpired) obj = StoredObject{};
  return Typed(std::move(obj), db, key, type);
}

Result<Collection> LoadCollectionForWrite(WriteTxn& txn, uint8_t db, std::string_view key,
                                          encoding::ValueType type, int64_t now_ms) {
  return Typed(LoadForWrite(txn, db, key, now_ms), db, key, type);
}

Status InitCollection(Collection* c, encoding::ValueType type, OidAllocator& oids) {
  auto oid = oids.Next();
  if (!oid.ok()) return oid.status();
  c->obj = StoredObject{};
  c->obj.state = ObjectState::kLive;
  c->obj.meta.type = type;
  c->obj.meta.oid = *oid;
  c->obj.meta.size = 0;
  c->stored = false;
  c->stored_meta.clear();
  return Status::Ok();
}

Status SaveCollection(WriteTxn& txn, Collection& c) {
  if (c.obj.meta.size > 0) {
    std::string record = encoding::EncodeMetadata(c.obj.meta);
    if (c.stored && record == c.stored_meta) {
      txn.NoteRewrite(ColumnFamily::kMeta, encoding::MetaKeyFor(c.db, c.key));
      return Status::Ok();
    }
    if (Status st = PutObjectMeta(txn, c.db, c.key, c.obj.meta); !st.ok()) return st;
    c.stored_meta = std::move(record);
    return Status::Ok();
  }
  c.stored_meta.clear();
  if (!c.stored) return Status::Ok();
  return DeleteEmptyObject(txn, c.db, c.key, c.obj.meta);
}

Result<Collection> OpenCollectionForWrite(WriteTxn& txn, OidAllocator& oids, uint8_t db, std::string_view key,
                                          encoding::ValueType type, int64_t now_ms) {
  auto c = LoadCollectionForWrite(txn, db, key, type, now_ms);
  if (!c.ok() || c->exists()) return c;
  if (Status st = InitCollection(&*c, type, oids); !st.ok()) return st;
  return c;
}

Result<std::optional<std::string>> GetElement(ReadView& view, const Collection& c, std::string_view field) {
  if (!c.exists()) return std::optional<std::string>();
  auto v = view.Get(ColumnFamily::kSubkey, c.SubkeyFor(field));
  if (v.ok()) return std::optional<std::string>(std::move(v).value());
  if (v.status().IsNotFound()) return std::optional<std::string>();
  return v.status();
}

Result<bool> HasElement(ReadView& view, const Collection& c, std::string_view field) {
  auto v = GetElement(view, c, field);
  if (!v.ok()) return v.status();
  return v->has_value();
}

Result<bool> PutElement(WriteTxn& txn, Collection& c, std::string_view field, std::string_view value) {
  auto had = HasElement(txn, c, field);
  if (!had.ok()) return had.status();
  if (Status st = txn.Put(ColumnFamily::kSubkey, c.SubkeyFor(field), value); !st.ok()) return st;
  if (!*had) ++c.obj.meta.size;
  return !*had;
}

Result<bool> RemoveElement(WriteTxn& txn, Collection& c, std::string_view field) {
  auto had = HasElement(txn, c, field);
  if (!had.ok()) return had.status();
  if (!*had) return false;
  if (Status st = txn.Delete(ColumnFamily::kSubkey, c.SubkeyFor(field)); !st.ok()) return st;
  --c.obj.meta.size;
  return true;
}

Status ForEachElement(ReadView& view, const Collection& c, const ElementFn& fn) {
  if (!c.exists()) return Status::Ok();
  auto it = ElementIterator(view, c);
  if (!it.ok()) return it.status();
  const size_t skip = c.Prefix().size();
  Status st = (*it)->SeekToFirst();
  for (; st.ok() && (*it)->Valid(); st = (*it)->Next()) {
    if (!fn((*it)->key().substr(skip), (*it)->value())) return Status::Ok();
  }
  return st;
}

Status IterateRange(ReadView& view, const Collection& c, const std::vector<uint64_t>& sorted_positions,
                    const std::function<void(std::string_view, std::string_view)>& fn) {
  if (sorted_positions.empty()) return Status::Ok();
  size_t next = 0;
  uint64_t index = 0;
  Status st = ForEachElement(view, c, [&](std::string_view f, std::string_view v) {
    while (next < sorted_positions.size() && sorted_positions[next] == index) {
      fn(f, v);
      ++next;
    }
    ++index;
    return next < sorted_positions.size();
  });
  if (!st.ok()) return st;
  if (next < sorted_positions.size())
    return Status::Corruption("collection holds fewer elements than its size");
  return Status::Ok();
}

Result<std::vector<Element>> RandomElements(ReadView& view, const Collection& c, int64_t count,
                                            std::mt19937_64& rng) {
  const bool distinct = count >= 0;
  const uint64_t wanted = distinct ? static_cast<uint64_t>(count) : static_cast<uint64_t>(-count);
  const std::vector<uint64_t> positions = SamplePositions(c.size(), wanted, distinct, rng);
  std::vector<Element> out;
  out.reserve(positions.size());
  Status st = IterateRange(view, c, positions, [&](std::string_view f, std::string_view v) {
    out.push_back({std::string(f), std::string(v)});
  });
  if (!st.ok()) return st;
  if (!distinct) std::shuffle(out.begin(), out.end(), rng);
  return out;
}

}  // namespace tideskv
