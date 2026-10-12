#include "types/hash_type.h"

#include <cmath>
#include <limits>

#include "common/number_format.h"
#include "common/string_util.h"
#include "types/collection.h"
#include "types/errors.h"

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

Result<std::vector<std::string>> HashGetAll(ReadView& view, uint8_t db, std::string_view key, HashPart part,
                                            int64_t now_ms) {
  auto c = LoadCollection(view, db, key, kHash, now_ms);
  if (!c.ok()) return c.status();
  std::vector<std::string> out;
  out.reserve(part == HashPart::kBoth ? 2 * c->size() : c->size());
  Status st = ForEachElement(view, *c, [&](std::string_view f, std::string_view v) {
    if (part != HashPart::kValues) out.emplace_back(f);
    if (part != HashPart::kFields) out.emplace_back(v);
    return true;
  });
  if (!st.ok()) return st;
  return out;
}

Result<int64_t> HashIncrBy(WriteTxn& txn, OidAllocator& oids, uint8_t db, std::string_view key,
                           std::string_view field, int64_t delta, int64_t now_ms) {
  auto c = OpenForWrite(txn, oids, db, key, now_ms);
  if (!c.ok()) return c.status();
  auto cur = GetElement(txn, *c, field);
  if (!cur.ok()) return cur.status();
  int64_t value = 0;
  if (*cur && !ParseInt64(**cur, &value)) return Status::UserError(std::string(kErrHashValueNotInteger));
  if ((delta < 0 && value < 0 && delta < std::numeric_limits<int64_t>::min() - value) ||
      (delta > 0 && value > 0 && delta > std::numeric_limits<int64_t>::max() - value)) {
    return Status::UserError(std::string(kErrOverflow));
  }
  value += delta;
  if (auto put = PutElement(txn, *c, field, std::to_string(value)); !put.ok()) return put.status();
  if (Status st = SaveCollection(txn, *c); !st.ok()) return st;
  return value;
}

Result<std::string> HashIncrByFloat(WriteTxn& txn, OidAllocator& oids, uint8_t db, std::string_view key,
                                    std::string_view field, long double delta, int64_t now_ms) {
  auto c = OpenForWrite(txn, oids, db, key, now_ms);
  if (!c.ok()) return c.status();
  auto cur = GetElement(txn, *c, field);
  if (!cur.ok()) return cur.status();
  long double value = 0;
  if (*cur && !ParseLongDouble(**cur, &value)) return Status::UserError(std::string(kErrHashValueNotFloat));
  value += delta;
  if (std::isnan(value) || std::isinf(value)) return Status::UserError(std::string(kErrNanOrInfinity));
  std::string out = FormatLongDouble(value);
  if (auto put = PutElement(txn, *c, field, out); !put.ok()) return put.status();
  if (Status st = SaveCollection(txn, *c); !st.ok()) return st;
  return out;
}

Result<std::optional<std::string>> HashRandomField(ReadView& view, uint8_t db, std::string_view key,
                                                   std::mt19937_64& rng, int64_t now_ms) {
  auto c = LoadCollection(view, db, key, kHash, now_ms);
  if (!c.ok()) return c.status();
  auto picked = RandomElements(view, *c, 1, rng);
  if (!picked.ok()) return picked.status();
  if (picked->empty()) return std::optional<std::string>();
  return std::optional<std::string>(std::move((*picked)[0].field));
}

Result<std::vector<std::string>> HashRandomFields(ReadView& view, uint8_t db, std::string_view key,
                                                  int64_t count, bool with_values, std::mt19937_64& rng,
                                                  int64_t now_ms) {
  auto c = LoadCollection(view, db, key, kHash, now_ms);
  if (!c.ok()) return c.status();
  auto picked = RandomElements(view, *c, count, rng);
  if (!picked.ok()) return picked.status();
  std::vector<std::string> out;
  out.reserve(with_values ? 2 * picked->size() : picked->size());
  for (Element& e : *picked) {
    out.push_back(std::move(e.field));
    if (with_values) out.push_back(std::move(e.value));
  }
  return out;
}

}  // namespace tideskv
