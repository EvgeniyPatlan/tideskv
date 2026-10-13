#include "types/zset_pop.h"

#include <algorithm>

#include "types/collection.h"

namespace tideskv {
namespace {

constexpr auto kZSet = encoding::ValueType::kZSet;

Status PopFrom(WriteTxn& txn, Collection& c, bool max, uint64_t count, std::vector<ScoreMember>* out) {
  const uint64_t n = std::min<uint64_t>(count, c.size());
  if (n == 0) return Status::Ok();
  Status st = ForEachByScore(txn, c, max, [&](double s, std::string_view m) -> Result<bool> {
    out->push_back({s, std::string(m)});
    return out->size() < n;
  });
  if (!st.ok()) return st;
  for (const ScoreMember& p : *out) {  // the iterator is closed: safe to write now
    if (st = DeleteMember(txn, c, p.member, p.score); !st.ok()) return st;
  }
  return SaveCollection(txn, c);
}

}  // namespace

Result<int64_t> ZsetRemoveRange(WriteTxn& txn, uint8_t db, std::string_view key, const RangeSpec& spec,
                                int64_t now_ms) {
  auto c = LoadCollectionForWrite(txn, db, key, kZSet, now_ms);
  if (!c.ok()) return c.status();
  if (!c->exists()) return int64_t{0};
  RangeSpec all = spec;
  all.offset = 0;
  all.count = -1;
  auto items = ZsetRange(txn, db, key, all, now_ms);
  if (!items.ok()) return items.status();
  for (const ScoreMember& p : *items) {
    if (Status st = DeleteMember(txn, *c, p.member, p.score); !st.ok()) return st;
  }
  if (items->empty()) return int64_t{0};
  if (Status st = SaveCollection(txn, *c); !st.ok()) return st;
  return static_cast<int64_t>(items->size());
}

Result<std::optional<std::vector<ScoreMember>>> ZsetPop(WriteTxn& txn, uint8_t db, std::string_view key,
                                                        bool max, uint64_t count, int64_t now_ms) {
  using Out = std::optional<std::vector<ScoreMember>>;
  auto c = LoadCollectionForWrite(txn, db, key, kZSet, now_ms);
  if (!c.ok()) return c.status();
  if (!c->exists()) return Out();
  std::vector<ScoreMember> out;
  if (Status st = PopFrom(txn, *c, max, count, &out); !st.ok()) return st;
  return Out(std::move(out));
}

Result<std::optional<ZPoppedFrom>> ZsetPopFirst(WriteTxn& txn, uint8_t db,
                                                const std::vector<std::string_view>& keys, bool max,
                                                uint64_t count, int64_t now_ms) {
  using Out = std::optional<ZPoppedFrom>;
  for (std::string_view key : keys) {
    auto c = LoadCollectionForWrite(txn, db, key, kZSet, now_ms);
    if (!c.ok()) return c.status();
    if (!c->exists()) continue;
    ZPoppedFrom out{std::string(key), {}};
    if (Status st = PopFrom(txn, *c, max, count, &out.items); !st.ok()) return st;
    return Out(std::move(out));
  }
  return Out();
}

Result<std::vector<ScoreMember>> ZsetRandomMembers(ReadView& view, uint8_t db, std::string_view key,
                                                   int64_t count, std::mt19937_64& rng, int64_t now_ms) {
  auto c = LoadCollection(view, db, key, kZSet, now_ms);
  if (!c.ok()) return c.status();
  std::vector<ScoreMember> out;
  if (!c->exists() || count == 0) return out;
  if (count > 0 && static_cast<uint64_t>(count) >= c->size()) {  // Redis: the whole set from the tail
    Status st = ForEachByScore(view, *c, true, [&](double s, std::string_view m) -> Result<bool> {
      out.push_back({s, std::string(m)});
      return true;
    });
    if (!st.ok()) return st;
    return out;
  }
  auto picked = RandomElements(view, *c, count, rng);  // subkey: field = member, value = raw score
  if (!picked.ok()) return picked.status();
  out.reserve(picked->size());
  for (Element& e : *picked) {
    if (e.value.size() != 8) return Status::Corruption("bad zset score");
    out.push_back({DecodeRawScore(e.value), std::move(e.field)});
  }
  return out;
}

Result<std::optional<std::string>> ZsetRandomMember(ReadView& view, uint8_t db, std::string_view key,
                                                    std::mt19937_64& rng, int64_t now_ms) {
  auto picked = ZsetRandomMembers(view, db, key, 1, rng, now_ms);
  if (!picked.ok()) return picked.status();
  if (picked->empty()) return std::optional<std::string>();
  return std::optional<std::string>(std::move(picked->front().member));
}

}  // namespace tideskv
