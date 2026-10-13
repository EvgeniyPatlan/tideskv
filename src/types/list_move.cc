#include "types/list_move.h"

#include <algorithm>

#include "types/collection.h"

namespace tideskv {
namespace {

constexpr auto kList = encoding::ValueType::kList;

}  // namespace

Result<std::optional<std::string>> ListMove(WriteTxn& txn, OidAllocator& oids, uint8_t db,
                                            std::string_view src, std::string_view dst, ListEnd from,
                                            ListEnd to, int64_t now_ms) {
  using Out = std::optional<std::string>;
  auto s = LoadCollectionForWrite(txn, db, src, kList, now_ms);
  if (!s.ok()) return s.status();
  if (!s->exists()) return Out();
  if (src == dst) {  // pop then push on the same collection: a rotation (or a no-op for LEFT LEFT)
    auto v = PopElement(txn, *s, from);
    if (!v.ok()) return v.status();
    if (Status st = PushElement(txn, *s, to, *v); !st.ok()) return st;
    if (Status st = SaveCollection(txn, *s); !st.ok()) return st;
    return Out(std::move(v).value());
  }
  auto d = LoadCollectionForWrite(txn, db, dst, kList, now_ms);  // WRONGTYPE before anything is popped
  if (!d.ok()) return d.status();
  auto v = PopElement(txn, *s, from);
  if (!v.ok()) return v.status();
  if (Status st = SaveCollection(txn, *s); !st.ok()) return st;
  if (!d->exists()) {
    if (Status st = InitCollection(&*d, kList, oids); !st.ok()) return st;
  }
  if (Status st = PushElement(txn, *d, to, *v); !st.ok()) return st;
  if (Status st = SaveCollection(txn, *d); !st.ok()) return st;
  return Out(std::move(v).value());
}

Result<std::optional<PoppedFrom>> ListPopFirst(WriteTxn& txn, uint8_t db,
                                               const std::vector<std::string_view>& keys, ListEnd end,
                                               uint64_t count, int64_t now_ms) {
  using Out = std::optional<PoppedFrom>;
  for (std::string_view key : keys) {
    auto c = LoadCollectionForWrite(txn, db, key, kList, now_ms);
    if (!c.ok()) return c.status();
    if (!c->exists()) continue;
    PoppedFrom out{std::string(key), {}};
    const uint64_t n = std::min<uint64_t>(count, c->size());
    out.values.reserve(n);
    for (uint64_t i = 0; i < n; ++i) {
      auto v = PopElement(txn, *c, end);
      if (!v.ok()) return v.status();
      out.values.push_back(std::move(v).value());
    }
    if (Status st = SaveCollection(txn, *c); !st.ok()) return st;
    return Out(std::move(out));
  }
  return Out();
}

}  // namespace tideskv
