#include "types/zset_query.h"

#include "encoding/key_layout.h"
#include "encoding/object_codec.h"
#include "types/collection.h"
#include "types/list_type.h"  // NormalizeListRange: Redis applies the same index rule to zset ranks
#include "types/object_store.h"

namespace tideskv {
namespace {

constexpr auto kZSet = encoding::ValueType::kZSet;

Status ByRank(ReadView& view, const Collection& c, const RangeSpec& s, std::vector<ScoreMember>* out) {
  uint64_t from = 0, to = 0;
  if (!NormalizeListRange(s.start, s.stop, c.size(), &from, &to)) return Status::Ok();
  uint64_t index = 0;
  return ForEachByScore(view, c, s.reverse, [&](double score, std::string_view m) -> Result<bool> {
    if (index++ < from) return true;
    out->push_back({score, std::string(m)});
    return index <= to;
  });
}

// LIMIT handling shared by BYSCORE and BYLEX.
ScoredFn Limited(const RangeSpec& s, std::vector<ScoreMember>* out, int64_t* skipped) {
  return [&s, out, skipped](double score, std::string_view m) -> Result<bool> {
    if ((*skipped)++ < s.offset) return true;
    out->push_back({score, std::string(m)});
    return s.count < 0 || static_cast<int64_t>(out->size()) < s.count;
  };
}

}  // namespace

Result<std::vector<ScoreMember>> ZsetRange(ReadView& view, uint8_t db, std::string_view key,
                                           const RangeSpec& s, int64_t now_ms) {
  auto c = LoadCollection(view, db, key, kZSet, now_ms);
  if (!c.ok()) return c.status();
  std::vector<ScoreMember> out;
  if (!c->exists()) return out;
  if (s.by == RangeBy::kRank) {
    if (Status st = ByRank(view, *c, s, &out); !st.ok()) return st;
    return out;
  }
  if (s.offset < 0 || s.count == 0) return out;
  int64_t skipped = 0;
  const ScoredFn fn = Limited(s, &out, &skipped);
  const Status st = s.by == RangeBy::kScore ? ForEachInScoreRange(view, *c, s.score, s.reverse, fn)
                                            : ForEachInLexRange(view, *c, s.lex, s.reverse, fn);
  if (!st.ok()) return st;
  return out;
}

Status StoreZset(WriteTxn& txn, OidAllocator& oids, uint8_t db, std::string_view dst,
                 const std::vector<ScoreMember>& items, int64_t now_ms) {
  auto old = LoadForWrite(txn, db, dst, now_ms);
  if (!old.ok()) return old.status();
  if (old->live()) {
    if (Status st = DeleteObject(txn, db, dst, old->meta); !st.ok()) return st;
  }
  if (items.empty()) return Status::Ok();
  Collection c;
  c.db = db;
  c.key = std::string(dst);
  c.slot = encoding::LayoutSlot(dst);
  if (Status st = InitCollection(&c, kZSet, oids); !st.ok()) return st;
  for (const ScoreMember& p : items) {
    auto prev = MemberScore(txn, c, p.member);
    if (!prev.ok()) return prev.status();
    if (Status st = PutMember(txn, c, p.member, p.score, *prev); !st.ok()) return st;
  }
  return SaveCollection(txn, c);
}

Result<int64_t> ZsetRangeStore(WriteTxn& txn, OidAllocator& oids, uint8_t db, std::string_view dst,
                               std::string_view src, const RangeSpec& spec, int64_t now_ms) {
  auto items = ZsetRange(txn, db, src, spec, now_ms);  // computed first: dst may be src
  if (!items.ok()) return items.status();
  if (Status st = StoreZset(txn, oids, db, dst, *items, now_ms); !st.ok()) return st;
  return static_cast<int64_t>(items->size());
}

Result<std::optional<RankResult>> ZsetRank(ReadView& view, uint8_t db, std::string_view key,
                                           std::string_view member, bool reverse, int64_t now_ms) {
  using Out = std::optional<RankResult>;
  auto c = LoadCollection(view, db, key, kZSet, now_ms);
  if (!c.ok()) return c.status();
  auto score = MemberScore(view, *c, member);
  if (!score.ok()) return score.status();
  const auto& member_score = *score;
  if (!member_score) return Out();
  // O(rank): count the members ordered before (score, member).
  const std::string target =
      encoding::EncodeZScoreKey(c->db, c->slot, c->obj.meta.oid, *member_score, member);
  auto it = view.NewIterator(ColumnFamily::kZScore,
                             {encoding::ZScorePrefix(c->db, c->slot, c->obj.meta.oid), target});
  if (!it.ok()) return it.status();
  int64_t before = 0;
  Status st = (*it)->SeekToFirst();
  for (; st.ok() && (*it)->Valid(); st = (*it)->Next()) ++before;
  if (!st.ok()) return st;
  const int64_t rank = reverse ? static_cast<int64_t>(c->size()) - 1 - before : before;
  return Out(RankResult{rank, *member_score});
}

}  // namespace tideskv
