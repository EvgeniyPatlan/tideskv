#include "types/list_edit.h"

#include <algorithm>
#include <functional>
#include <string>

#include "encoding/object_codec.h"
#include "types/collection.h"
#include "types/list_type.h"

namespace tideskv {
namespace {

constexpr auto kList = encoding::ValueType::kList;
constexpr uint64_t kBatch = 1024;  // elements per iterator pass (as M3 RehomeEntries)

std::string KeyAt(const Collection& c, uint64_t index) {
  return c.SubkeyFor(encoding::EncodeListIndex(index));
}

// Reads the values at absolute indexes [lo, hi] (ascending), closing the iterator before returning.
Status ReadIndexes(ReadView& view, const Collection& c, uint64_t lo, uint64_t hi,
                   std::vector<std::string>* out) {
  out->clear();
  auto it = view.NewIterator(ColumnFamily::kSubkey, {KeyAt(c, lo), KeyAt(c, hi + 1)});
  if (!it.ok()) return it.status();
  Status st = (*it)->SeekToFirst();
  for (; st.ok() && (*it)->Valid(); st = (*it)->Next()) out->emplace_back((*it)->value());
  if (!st.ok()) return st;
  if (out->size() != hi - lo + 1) return Status::Corruption("list holds fewer elements than its size");
  return Status::Ok();
}

// Moves the elements at absolute indexes [first, last] by delta (-1 or +1); see the task notes.
Status ShiftRange(WriteTxn& txn, const Collection& c, uint64_t first, uint64_t last, int delta) {
  if (first > last) return Status::Ok();
  std::vector<std::string> batch;
  if (delta < 0) {
    for (uint64_t lo = first; lo <= last; lo += kBatch) {
      const uint64_t hi = std::min(last, lo + kBatch - 1);
      if (Status st = ReadIndexes(txn, c, lo, hi, &batch); !st.ok()) return st;
      for (uint64_t i = 0; i < batch.size(); ++i) {
        if (Status st = txn.Put(ColumnFamily::kSubkey, KeyAt(c, lo + i - 1), batch[i]); !st.ok()) return st;
      }
    }
    return Status::Ok();
  }
  for (uint64_t hi = last + 1; hi > first;) {  // hi is one past the batch, walking down
    const uint64_t lo = hi - first > kBatch ? hi - kBatch : first;
    if (Status st = ReadIndexes(txn, c, lo, hi - 1, &batch); !st.ok()) return st;
    for (uint64_t i = batch.size(); i-- > 0;) {
      if (Status st = txn.Put(ColumnFamily::kSubkey, KeyAt(c, lo + i + 1), batch[i]); !st.ok()) return st;
    }
    hi = lo;
  }
  return Status::Ok();
}

// Visits every element left to right (or right to left), batch by batch; fn returns false to stop.
// Positions are logical (0 = leftmost). Writes made by fn must only touch already-visited slots.
using Visitor = std::function<Result<bool>(uint64_t position, const std::string& value)>;
Status Walk(ReadView& view, const Collection& c, bool from_right, const Visitor& fn) {
  const uint64_t size = c.obj.meta.size, head = c.obj.meta.head;
  std::vector<std::string> batch;
  for (uint64_t done = 0; done < size; done += batch.size()) {
    const uint64_t n = std::min(kBatch, size - done);
    const uint64_t lo = from_right ? size - done - n : done;  // logical positions [lo, lo + n)
    if (Status st = ReadIndexes(view, c, head + lo, head + lo + n - 1, &batch); !st.ok()) return st;
    for (uint64_t k = 0; k < n; ++k) {
      const uint64_t i = from_right ? n - 1 - k : k;
      auto more = fn(lo + i, batch[i]);
      if (!more.ok()) return more.status();
      if (!*more) return Status::Ok();
    }
  }
  return Status::Ok();
}

Result<std::optional<uint64_t>> FindPivot(ReadView& view, const Collection& c, std::string_view pivot) {
  std::optional<uint64_t> found;
  Status st = Walk(view, c, false, [&](uint64_t pos, const std::string& v) -> Result<bool> {
    if (v != pivot) return true;
    found = pos;
    return false;
  });
  if (!st.ok()) return st;
  return found;
}

// Deletes the elements at absolute indexes [lo, hi] without reading them.
Status DeleteIndexes(WriteTxn& txn, const Collection& c, uint64_t lo, uint64_t hi) {
  for (uint64_t i = lo; i <= hi && i >= lo; ++i) {
    if (Status st = txn.Delete(ColumnFamily::kSubkey, KeyAt(c, i)); !st.ok()) return st;
  }
  return Status::Ok();
}

}  // namespace

Result<int64_t> ListInsert(WriteTxn& txn, uint8_t db, std::string_view key, InsertWhere where,
                           std::string_view pivot, std::string_view value, int64_t now_ms) {
  auto c = LoadCollectionForWrite(txn, db, key, kList, now_ms);
  if (!c.ok()) return c.status();
  if (!c->exists()) return int64_t{0};
  auto found = FindPivot(txn, *c, pivot);
  if (!found.ok()) return found.status();
  const auto& pivot_pos = *found;
  if (!pivot_pos) return int64_t{-1};
  encoding::Metadata& m = c->obj.meta;
  const uint64_t q = *pivot_pos + (where == InsertWhere::kAfter ? 1 : 0);  // logical slot of the new element
  uint64_t slot = 0;
  if (q <= m.size - q) {  // fewer elements on the left: move [0, q) one step left
    if (q > 0) {
      if (Status st = ShiftRange(txn, *c, m.head, m.head + q - 1, -1); !st.ok()) return st;
    }
    --m.head;
    slot = m.head + q;
  } else {  // move [q, size) one step right
    if (Status st = ShiftRange(txn, *c, m.head + q, m.tail, +1); !st.ok()) return st;
    ++m.tail;
    slot = m.head + q;
  }
  if (Status st = txn.Put(ColumnFamily::kSubkey, KeyAt(*c, slot), value); !st.ok()) return st;
  ++m.size;
  if (Status st = SaveCollection(txn, *c); !st.ok()) return st;
  return static_cast<int64_t>(m.size);
}

Result<int64_t> ListRemove(WriteTxn& txn, uint8_t db, std::string_view key, int64_t count,
                           std::string_view value, int64_t now_ms) {
  auto c = LoadCollectionForWrite(txn, db, key, kList, now_ms);
  if (!c.ok()) return c.status();
  if (!c->exists()) return int64_t{0};
  const bool from_right = count < 0;
  const uint64_t limit = count == 0 ? UINT64_MAX : (from_right ? 0 - static_cast<uint64_t>(count) : count);
  const uint64_t head = c->obj.meta.head;
  uint64_t removed = 0;
  // Survivors slide towards the scan's starting end by `removed` slots, into already-read slots.
  Status st = Walk(txn, *c, from_right, [&](uint64_t pos, const std::string& v) -> Result<bool> {
    if (removed < limit && v == value) {
      ++removed;
      return true;
    }
    if (removed == 0) return true;
    const uint64_t to = from_right ? pos + removed : pos - removed;
    if (Status put = txn.Put(ColumnFamily::kSubkey, KeyAt(*c, head + to), v); !put.ok()) return put;
    return true;
  });
  if (!st.ok()) return st;
  if (removed == 0) return int64_t{0};
  encoding::Metadata& m = c->obj.meta;
  if (from_right) {
    if (st = DeleteIndexes(txn, *c, m.head, m.head + removed - 1); !st.ok()) return st;
    m.head += removed;
  } else {
    if (st = DeleteIndexes(txn, *c, m.tail - removed + 1, m.tail); !st.ok()) return st;
    m.tail -= removed;
  }
  m.size -= removed;
  if (st = SaveCollection(txn, *c); !st.ok()) return st;
  return static_cast<int64_t>(removed);
}

Status ListTrim(WriteTxn& txn, uint8_t db, std::string_view key, int64_t start, int64_t stop,
                int64_t now_ms) {
  auto c = LoadCollectionForWrite(txn, db, key, kList, now_ms);
  if (!c.ok()) return c.status();
  if (!c->exists()) return Status::Ok();
  encoding::Metadata& m = c->obj.meta;
  uint64_t from = 0, to = 0;
  if (!NormalizeListRange(start, stop, m.size, &from, &to)) {
    if (Status st = DeleteIndexes(txn, *c, m.head, m.tail); !st.ok()) return st;
    m.size = 0;
    return SaveCollection(txn, *c);  // deletes the key (no prefix tombstone)
  }
  if (from > 0) {
    if (Status st = DeleteIndexes(txn, *c, m.head, m.head + from - 1); !st.ok()) return st;
  }
  if (to + 1 < m.size) {
    if (Status st = DeleteIndexes(txn, *c, m.head + to + 1, m.tail); !st.ok()) return st;
  }
  if (from == 0 && to + 1 == m.size) return Status::Ok();
  m.tail = m.head + to;
  m.head += from;
  m.size = to - from + 1;
  return SaveCollection(txn, *c);
}

Result<std::optional<std::vector<int64_t>>> ListPos(ReadView& view, uint8_t db, std::string_view key,
                                                    std::string_view value, const ListPosOptions& opts,
                                                    int64_t now_ms) {
  using Out = std::optional<std::vector<int64_t>>;
  auto c = LoadCollection(view, db, key, kList, now_ms);
  if (!c.ok()) return c.status();
  if (!c->exists()) return Out();
  const bool from_right = opts.rank < 0;
  const uint64_t skip = (from_right ? 0 - static_cast<uint64_t>(opts.rank) : opts.rank) - 1;
  const uint64_t want =
      !opts.count ? 1 : (*opts.count == 0 ? UINT64_MAX : static_cast<uint64_t>(*opts.count));
  std::vector<int64_t> out;
  uint64_t seen = 0, matches = 0;
  Status st = Walk(view, *c, from_right, [&](uint64_t pos, const std::string& v) -> Result<bool> {
    if (opts.maxlen != 0 && seen++ >= static_cast<uint64_t>(opts.maxlen)) return false;
    if (v == value && matches++ >= skip) out.push_back(static_cast<int64_t>(pos));
    return out.size() < want;
  });
  if (!st.ok()) return st;
  return Out(std::move(out));
}

}  // namespace tideskv
