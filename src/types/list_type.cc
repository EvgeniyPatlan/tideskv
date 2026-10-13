#include "types/list_type.h"

#include <algorithm>

#include "common/string_util.h"
#include "encoding/object_codec.h"
#include "types/errors.h"

namespace tideskv {
namespace {

constexpr auto kList = encoding::ValueType::kList;

Status MissingElement() { return Status::Corruption("list holds fewer elements than its size"); }

// Index argument -> position, or nullopt when outside [-len, len).
std::optional<uint64_t> PositionOf(int64_t index, uint64_t len) {
  const auto n = static_cast<int64_t>(len);  // a list never holds 2^63 elements
  if (index < 0) index += n;
  if (index < 0 || index >= n) return std::nullopt;
  return static_cast<uint64_t>(index);
}

}  // namespace

std::string ListElementKey(const Collection& c, uint64_t position) {
  return c.SubkeyFor(encoding::EncodeListIndex(c.obj.meta.head + position));
}

Result<std::string> ListElementAt(ReadView& view, const Collection& c, uint64_t position) {
  auto v = view.Get(ColumnFamily::kSubkey, ListElementKey(c, position));
  if (!v.ok() && v.status().IsNotFound()) return MissingElement();
  return v;
}

Result<std::string> PopElement(WriteTxn& txn, Collection& c, ListEnd end) {
  encoding::Metadata& m = c.obj.meta;
  const uint64_t position = end == ListEnd::kLeft ? 0 : m.size - 1;
  auto v = ListElementAt(txn, c, position);
  if (!v.ok()) return v.status();
  if (Status st = txn.Delete(ColumnFamily::kSubkey, ListElementKey(c, position)); !st.ok()) return st;
  if (end == ListEnd::kLeft) {
    ++m.head;
  } else {
    --m.tail;
  }
  --m.size;
  return v;
}

Status PushElement(WriteTxn& txn, Collection& c, ListEnd end, std::string_view value) {
  encoding::Metadata& m = c.obj.meta;
  const uint64_t index = end == ListEnd::kLeft ? --m.head : ++m.tail;
  if (Status st = txn.Put(ColumnFamily::kSubkey, c.SubkeyFor(encoding::EncodeListIndex(index)), value);
      !st.ok())
    return st;
  ++m.size;
  return Status::Ok();
}

Status ReadListRange(ReadView& view, const Collection& c, uint64_t from, uint64_t to,
                     std::vector<std::string>* out) {
  auto it = view.NewIterator(ColumnFamily::kSubkey, {ListElementKey(c, from), ListElementKey(c, to + 1)});
  if (!it.ok()) return it.status();
  const uint64_t want = to - from + 1;
  uint64_t got = 0;
  Status st = (*it)->SeekToFirst();
  for (; st.ok() && (*it)->Valid(); st = (*it)->Next()) {
    out->emplace_back((*it)->value());
    ++got;
  }
  if (!st.ok()) return st;
  return got == want ? Status::Ok() : MissingElement();
}

bool NormalizeListRange(int64_t start, int64_t stop, uint64_t len, uint64_t* from, uint64_t* to) {
  const auto n = static_cast<int64_t>(len);
  if (start < 0) start += n;
  if (stop < 0) stop += n;
  if (start < 0) start = 0;
  if (start > stop || start >= n) return false;
  if (stop >= n) stop = n - 1;
  *from = static_cast<uint64_t>(start);
  *to = static_cast<uint64_t>(stop);
  return true;
}

Result<int64_t> ListPush(WriteTxn& txn, OidAllocator& oids, uint8_t db, std::string_view key, ListEnd end,
                         const std::vector<std::string_view>& values, bool only_if_exists, int64_t now_ms) {
  auto c = only_if_exists ? LoadCollectionForWrite(txn, db, key, kList, now_ms)
                          : OpenCollectionForWrite(txn, oids, db, key, kList, now_ms);
  if (!c.ok()) return c.status();
  if (!c->exists()) return int64_t{0};
  for (std::string_view v : values) {
    if (Status st = PushElement(txn, *c, end, v); !st.ok()) return st;
  }
  if (Status st = SaveCollection(txn, *c); !st.ok()) return st;
  return static_cast<int64_t>(c->obj.meta.size);
}

Result<std::optional<std::vector<std::string>>> ListPop(WriteTxn& txn, uint8_t db, std::string_view key,
                                                        ListEnd end, uint64_t count, int64_t now_ms) {
  using Out = std::optional<std::vector<std::string>>;
  auto c = LoadCollectionForWrite(txn, db, key, kList, now_ms);
  if (!c.ok()) return c.status();
  if (!c->exists()) return Out();
  const uint64_t n = std::min<uint64_t>(count, c->obj.meta.size);
  std::vector<std::string> out;
  out.reserve(n);
  for (uint64_t i = 0; i < n; ++i) {
    auto v = PopElement(txn, *c, end);
    if (!v.ok()) return v.status();
    out.push_back(std::move(v).value());
  }
  if (n > 0) {
    if (Status st = SaveCollection(txn, *c); !st.ok()) return st;
  }
  return Out(std::move(out));
}

Result<int64_t> ListLen(ReadView& view, uint8_t db, std::string_view key, int64_t now_ms) {
  auto c = LoadCollection(view, db, key, kList, now_ms);
  if (!c.ok()) return c.status();
  return static_cast<int64_t>(c->size());
}

Result<std::optional<std::string>> ListIndex(ReadView& view, uint8_t db, std::string_view key,
                                             std::string_view index_arg, int64_t now_ms) {
  using Out = std::optional<std::string>;
  auto c = LoadCollection(view, db, key, kList, now_ms);
  if (!c.ok()) return c.status();
  if (!c->exists()) return Out();
  int64_t index = 0;
  if (!ParseInt64(index_arg, &index)) return Status::UserError(std::string(kErrNotInteger));
  const std::optional<uint64_t> pos = PositionOf(index, c->size());
  if (!pos) return Out();
  auto v = ListElementAt(view, *c, *pos);
  if (!v.ok()) return v.status();
  return Out(std::move(v).value());
}

Status ListSet(WriteTxn& txn, uint8_t db, std::string_view key, std::string_view index_arg,
               std::string_view value, int64_t now_ms) {
  auto c = LoadCollectionForWrite(txn, db, key, kList, now_ms);
  if (!c.ok()) return c.status();
  if (!c->exists()) return Status::UserError(std::string(kErrNoSuchKey));
  int64_t index = 0;
  if (!ParseInt64(index_arg, &index)) return Status::UserError(std::string(kErrNotInteger));
  const std::optional<uint64_t> pos = PositionOf(index, c->size());
  if (!pos) return Status::UserError(std::string(kErrIndexOutOfRange));
  return txn.Put(ColumnFamily::kSubkey, ListElementKey(*c, *pos), value);
}

Result<std::vector<std::string>> ListRange(ReadView& view, uint8_t db, std::string_view key, int64_t start,
                                           int64_t stop, int64_t now_ms) {
  auto c = LoadCollection(view, db, key, kList, now_ms);
  if (!c.ok()) return c.status();
  std::vector<std::string> out;
  uint64_t from = 0, to = 0;
  if (!c->exists() || !NormalizeListRange(start, stop, c->size(), &from, &to)) return out;
  if (Status st = ReadListRange(view, *c, from, to, &out); !st.ok()) return st;
  return out;
}

}  // namespace tideskv
