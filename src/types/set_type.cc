#include "types/set_type.h"

#include "types/collection.h"

namespace tideskv {
namespace {

constexpr auto kSet = encoding::ValueType::kSet;

}  // namespace

Result<int64_t> SetAdd(WriteTxn& txn, OidAllocator& oids, uint8_t db, std::string_view key,
                       const std::vector<std::string_view>& members, int64_t now_ms) {
  auto c = OpenCollectionForWrite(txn, oids, db, key, kSet, now_ms);
  if (!c.ok()) return c.status();
  int64_t added = 0;
  for (std::string_view m : members) {
    auto had = HasElement(txn, *c, m);  // sees this txn's earlier adds: duplicates count once
    if (!had.ok()) return had.status();
    if (*had) continue;
    if (Status st = txn.Put(ColumnFamily::kSubkey, c->SubkeyFor(m), {}); !st.ok()) return st;
    ++c->obj.meta.size;
    ++added;
  }
  if (added == 0) return added;
  if (Status st = SaveCollection(txn, *c); !st.ok()) return st;
  return added;
}

Result<int64_t> SetRemove(WriteTxn& txn, uint8_t db, std::string_view key,
                          const std::vector<std::string_view>& members, int64_t now_ms) {
  auto c = LoadCollectionForWrite(txn, db, key, kSet, now_ms);
  if (!c.ok()) return c.status();
  if (!c->exists()) return int64_t{0};
  int64_t removed = 0;
  for (std::string_view m : members) {
    auto gone = RemoveElement(txn, *c, m);
    if (!gone.ok()) return gone.status();
    if (*gone) ++removed;
  }
  if (removed == 0) return removed;
  if (Status st = SaveCollection(txn, *c); !st.ok()) return st;
  return removed;
}

Result<int64_t> SetCard(ReadView& view, uint8_t db, std::string_view key, int64_t now_ms) {
  auto c = LoadCollection(view, db, key, kSet, now_ms);
  if (!c.ok()) return c.status();
  return static_cast<int64_t>(c->size());
}

Result<bool> SetIsMember(ReadView& view, uint8_t db, std::string_view key, std::string_view member,
                         int64_t now_ms) {
  auto c = LoadCollection(view, db, key, kSet, now_ms);
  if (!c.ok()) return c.status();
  return HasElement(view, *c, member);
}

Result<std::vector<int64_t>> SetMultiIsMember(ReadView& view, uint8_t db, std::string_view key,
                                              const std::vector<std::string_view>& members, int64_t now_ms) {
  auto c = LoadCollection(view, db, key, kSet, now_ms);
  if (!c.ok()) return c.status();
  std::vector<int64_t> out;
  out.reserve(members.size());
  for (std::string_view m : members) {
    auto has = HasElement(view, *c, m);
    if (!has.ok()) return has.status();
    out.push_back(*has ? 1 : 0);
  }
  return out;
}

Result<std::vector<std::string>> SetMembers(ReadView& view, uint8_t db, std::string_view key,
                                            int64_t now_ms) {
  auto c = LoadCollection(view, db, key, kSet, now_ms);
  if (!c.ok()) return c.status();
  std::vector<std::string> out;
  out.reserve(c->size());
  Status st = ForEachElement(view, *c, [&](std::string_view m, std::string_view) {
    out.emplace_back(m);
    return true;
  });
  if (!st.ok()) return st;
  return out;
}

Result<std::vector<std::string>> SetPop(WriteTxn& txn, uint8_t db, std::string_view key, uint64_t count,
                                        std::mt19937_64& rng, int64_t now_ms) {
  auto c = LoadCollectionForWrite(txn, db, key, kSet, now_ms);
  if (!c.ok()) return c.status();
  std::vector<std::string> out;
  if (!c->exists() || count == 0) return out;
  if (count >= c->size()) {  // the whole set, in byte order; one prefix tombstone like Redis's dbDelete
    out.reserve(c->size());
    Status st = ForEachElement(txn, *c, [&](std::string_view m, std::string_view) {
      out.emplace_back(m);
      return true;
    });
    if (!st.ok()) return st;
    if (st = DeleteObject(txn, db, key, c->obj.meta); !st.ok()) return st;
    return out;
  }
  auto picked = RandomElements(txn, *c, static_cast<int64_t>(count), rng);
  if (!picked.ok()) return picked.status();
  out.reserve(picked->size());
  for (Element& e : *picked) {
    if (auto gone = RemoveElement(txn, *c, e.field); !gone.ok()) return gone.status();
    out.push_back(std::move(e.field));
  }
  if (Status st = SaveCollection(txn, *c); !st.ok()) return st;
  return out;
}

Result<std::optional<std::string>> SetPopOne(WriteTxn& txn, uint8_t db, std::string_view key,
                                             std::mt19937_64& rng, int64_t now_ms) {
  auto popped = SetPop(txn, db, key, 1, rng, now_ms);
  if (!popped.ok()) return popped.status();
  if (popped->empty()) return std::optional<std::string>();
  return std::optional<std::string>(std::move((*popped)[0]));
}

Result<std::vector<std::string>> SetRandomMembers(ReadView& view, uint8_t db, std::string_view key,
                                                  int64_t count, std::mt19937_64& rng, int64_t now_ms) {
  auto c = LoadCollection(view, db, key, kSet, now_ms);
  if (!c.ok()) return c.status();
  auto picked = RandomElements(view, *c, count, rng);
  if (!picked.ok()) return picked.status();
  std::vector<std::string> out;
  out.reserve(picked->size());
  for (Element& e : *picked) out.push_back(std::move(e.field));
  return out;
}

Result<std::optional<std::string>> SetRandomMember(ReadView& view, uint8_t db, std::string_view key,
                                                   std::mt19937_64& rng, int64_t now_ms) {
  auto picked = SetRandomMembers(view, db, key, 1, rng, now_ms);
  if (!picked.ok()) return picked.status();
  if (picked->empty()) return std::optional<std::string>();
  return std::optional<std::string>(std::move((*picked)[0]));
}

Result<int64_t> SetMove(WriteTxn& txn, OidAllocator& oids, uint8_t db, std::string_view src,
                        std::string_view dst, std::string_view member, int64_t now_ms) {
  auto from = LoadCollectionForWrite(txn, db, src, kSet, now_ms);  // errors only for a live non-set
  if (!from.ok()) return from.status();
  if (!from->exists()) return int64_t{0};  // before dst's type is looked at, as in Redis
  auto to = LoadCollectionForWrite(txn, db, dst, kSet, now_ms);
  if (!to.ok()) return to.status();
  if (src == dst) {
    auto has = HasElement(txn, *from, member);
    if (!has.ok()) return has.status();
    return int64_t{*has ? 1 : 0};
  }
  auto gone = RemoveElement(txn, *from, member);
  if (!gone.ok()) return gone.status();
  if (!*gone) return int64_t{0};
  if (Status st = SaveCollection(txn, *from); !st.ok()) return st;
  if (!to->exists()) {
    if (Status st = InitCollection(&*to, kSet, oids); !st.ok()) return st;
  }
  auto had = HasElement(txn, *to, member);
  if (!had.ok()) return had.status();
  if (!*had) {
    if (Status st = txn.Put(ColumnFamily::kSubkey, to->SubkeyFor(member), {}); !st.ok()) return st;
    ++to->obj.meta.size;
    if (Status st = SaveCollection(txn, *to); !st.ok()) return st;
  }
  return int64_t{1};
}

}  // namespace tideskv
