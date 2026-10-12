#include "types/set_algebra.h"

#include <algorithm>
#include <set>

#include "common/crc16.h"
#include "types/collection.h"
#include "types/object_store.h"

namespace tideskv {
namespace {

constexpr auto kSet = encoding::ValueType::kSet;

Result<std::vector<Collection>> LoadAll(ReadView& view, uint8_t db, const std::vector<std::string_view>& keys,
                                        int64_t now_ms) {
  std::vector<Collection> sets;
  sets.reserve(keys.size());
  for (std::string_view key : keys) {
    auto c = LoadCollection(view, db, key, kSet, now_ms);
    if (!c.ok()) return c.status();
    sets.push_back(std::move(c).value());
  }
  return sets;
}

// Members of sets[0] that every other set holds, walking the smallest set; stops at limit.
Result<std::vector<std::string>> Intersect(ReadView& view, std::vector<Collection> sets, uint64_t limit) {
  std::vector<std::string> out;
  if (std::any_of(sets.begin(), sets.end(), [](const Collection& c) { return !c.exists(); })) return out;
  std::sort(sets.begin(), sets.end(),
            [](const Collection& x, const Collection& y) { return x.size() < y.size(); });
  Status inner;
  Status st = ForEachElement(view, sets[0], [&](std::string_view m, std::string_view) {
    for (size_t i = 1; i < sets.size(); ++i) {
      auto has = HasElement(view, sets[i], m);
      if (!has.ok()) {
        inner = has.status();
        return false;
      }
      if (!*has) return true;
    }
    out.emplace_back(m);
    return limit == 0 || out.size() < limit;
  });
  if (!st.ok()) return st;
  if (!inner.ok()) return inner;
  return out;  // the smallest set's byte order
}

Result<std::vector<std::string>> Union(ReadView& view, const std::vector<Collection>& sets) {
  std::set<std::string> merged;
  for (const Collection& c : sets) {
    Status st = ForEachElement(view, c, [&](std::string_view m, std::string_view) {
      merged.emplace(m);
      return true;
    });
    if (!st.ok()) return st;
  }
  return std::vector<std::string>(merged.begin(), merged.end());
}

Result<std::vector<std::string>> Difference(ReadView& view, const std::vector<Collection>& sets) {
  std::vector<std::string> out;
  Status inner;
  Status st = ForEachElement(view, sets[0], [&](std::string_view m, std::string_view) {
    for (size_t i = 1; i < sets.size(); ++i) {
      auto has = HasElement(view, sets[i], m);  // a missing set holds nothing
      if (!has.ok()) {
        inner = has.status();
        return false;
      }
      if (*has) return true;
    }
    out.emplace_back(m);
    return true;
  });
  if (!st.ok()) return st;
  if (!inner.ok()) return inner;
  return out;
}

}  // namespace

Result<std::vector<std::string>> SetCombine(ReadView& view, uint8_t db, SetOp op,
                                            const std::vector<std::string_view>& keys, int64_t now_ms) {
  auto sets = LoadAll(view, db, keys, now_ms);
  if (!sets.ok()) return sets.status();
  switch (op) {
    case SetOp::kInter:
      return Intersect(view, std::move(sets).value(), 0);
    case SetOp::kUnion:
      return Union(view, *sets);
    case SetOp::kDiff:
      return Difference(view, *sets);
  }
  return Status::Internal("unknown set operation");
}

Result<int64_t> SetCombineStore(WriteTxn& txn, OidAllocator& oids, uint8_t db, SetOp op, std::string_view dst,
                                const std::vector<std::string_view>& keys, int64_t now_ms) {
  auto members = SetCombine(txn, db, op, keys, now_ms);
  if (!members.ok()) return members.status();
  auto old = LoadForWrite(txn, db, dst, now_ms);
  if (!old.ok()) return old.status();
  if (old->live()) {
    if (Status st = DeleteObject(txn, db, dst, old->meta); !st.ok()) return st;
  }
  if (members->empty()) return int64_t{0};
  Collection c;
  c.db = db;
  c.key = std::string(dst);
  c.slot = KeySlot(dst);
  if (Status st = InitCollection(&c, kSet, oids); !st.ok()) return st;
  for (const std::string& m : *members) {
    if (Status st = txn.Put(ColumnFamily::kSubkey, c.SubkeyFor(m), {}); !st.ok()) return st;
  }
  c.obj.meta.size = members->size();
  if (Status st = SaveCollection(txn, c); !st.ok()) return st;
  return static_cast<int64_t>(members->size());
}

Result<int64_t> SetInterCard(ReadView& view, uint8_t db, const std::vector<std::string_view>& keys,
                             uint64_t limit, int64_t now_ms) {
  auto sets = LoadAll(view, db, keys, now_ms);
  if (!sets.ok()) return sets.status();
  auto inter = Intersect(view, std::move(sets).value(), limit);
  if (!inter.ok()) return inter.status();
  return static_cast<int64_t>(inter->size());
}

}  // namespace tideskv
