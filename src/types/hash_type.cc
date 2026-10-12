#include "types/hash_type.h"

#include "types/collection.h"

namespace tideskv {
namespace {

constexpr auto kHash = encoding::ValueType::kHash;

// The hash at key for writing; a missing one is initialised (fresh oid) but not yet stored.
Result<Collection> OpenForWrite(WriteTxn& txn, OidAllocator& oids, uint8_t db, std::string_view key,
                                int64_t now_ms) {
  return OpenCollectionForWrite(txn, oids, db, key, kHash, now_ms);
}

}  // namespace

Result<int64_t> HashSet(WriteTxn& txn, OidAllocator& oids, uint8_t db, std::string_view key,
                        const std::vector<FieldValue>& pairs, int64_t now_ms) {
  auto c = OpenForWrite(txn, oids, db, key, now_ms);
  if (!c.ok()) return c.status();
  int64_t added = 0;
  for (const auto& [field, value] : pairs) {
    auto created = PutElement(txn, *c, field, value);
    if (!created.ok()) return created.status();
    if (*created) ++added;
  }
  if (Status st = SaveCollection(txn, *c); !st.ok()) return st;
  return added;
}

Result<bool> HashSetNx(WriteTxn& txn, OidAllocator& oids, uint8_t db, std::string_view key,
                       std::string_view field, std::string_view value, int64_t now_ms) {
  auto c = OpenForWrite(txn, oids, db, key, now_ms);
  if (!c.ok()) return c.status();
  auto had = HasElement(txn, *c, field);
  if (!had.ok()) return had.status();
  if (*had) return false;
  if (auto created = PutElement(txn, *c, field, value); !created.ok()) return created.status();
  if (Status st = SaveCollection(txn, *c); !st.ok()) return st;
  return true;
}

Result<std::optional<std::string>> HashGet(ReadView& view, uint8_t db, std::string_view key,
                                           std::string_view field, int64_t now_ms) {
  auto c = LoadCollection(view, db, key, kHash, now_ms);
  if (!c.ok()) return c.status();
  return GetElement(view, *c, field);
}

Result<std::vector<std::optional<std::string>>> HashMultiGet(ReadView& view, uint8_t db, std::string_view key,
                                                             const std::vector<std::string_view>& fields,
                                                             int64_t now_ms) {
  auto c = LoadCollection(view, db, key, kHash, now_ms);
  if (!c.ok()) return c.status();
  std::vector<std::optional<std::string>> out;
  out.reserve(fields.size());
  for (std::string_view f : fields) {
    auto v = GetElement(view, *c, f);
    if (!v.ok()) return v.status();
    out.push_back(std::move(v).value());
  }
  return out;
}

Result<int64_t> HashDelete(WriteTxn& txn, uint8_t db, std::string_view key,
                           const std::vector<std::string_view>& fields, int64_t now_ms) {
  auto c = LoadCollectionForWrite(txn, db, key, kHash, now_ms);
  if (!c.ok()) return c.status();
  if (!c->exists()) return int64_t{0};
  int64_t removed = 0;
  for (std::string_view f : fields) {
    auto gone = RemoveElement(txn, *c, f);  // a repeated field is removed once
    if (!gone.ok()) return gone.status();
    if (*gone) ++removed;
  }
  if (removed == 0) return removed;
  if (Status st = SaveCollection(txn, *c); !st.ok()) return st;
  return removed;
}

Result<int64_t> HashLen(ReadView& view, uint8_t db, std::string_view key, int64_t now_ms) {
  auto c = LoadCollection(view, db, key, kHash, now_ms);
  if (!c.ok()) return c.status();
  return static_cast<int64_t>(c->size());
}

Result<int64_t> HashStrlen(ReadView& view, uint8_t db, std::string_view key, std::string_view field,
                           int64_t now_ms) {
  auto v = HashGet(view, db, key, field, now_ms);
  if (!v.ok()) return v.status();
  return *v ? static_cast<int64_t>((*v)->size()) : int64_t{0};
}

Result<bool> HashExists(ReadView& view, uint8_t db, std::string_view key, std::string_view field,
                        int64_t now_ms) {
  auto v = HashGet(view, db, key, field, now_ms);
  if (!v.ok()) return v.status();
  return v->has_value();
}

}  // namespace tideskv
