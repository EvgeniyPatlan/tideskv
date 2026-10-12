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

}  // namespace tideskv
