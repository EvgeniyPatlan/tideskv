#include "types/zset_type.h"

#include <bit>
#include <cmath>

#include "common/coding.h"
#include "common/score_format.h"
#include "encoding/key_codec.h"
#include "encoding/object_codec.h"
#include "encoding/score_codec.h"
#include "types/errors.h"

namespace tideskv {
namespace {

constexpr auto kZSet = encoding::ValueType::kZSet;

std::string OrderKey(const Collection& c, double score, std::string_view member) {
  return encoding::EncodeZScoreKey(c.db, c.slot, c.obj.meta.oid, score, member);
}

std::string OrderPrefix(const Collection& c) { return encoding::ZScorePrefix(c.db, c.slot, c.obj.meta.oid); }

// Iterates [lower, upper) of the zscore CF, forward or backward, decoding each entry.
Status Scan(ReadView& view, std::string lower, std::string upper, bool reverse, const ScoredFn& fn) {
  auto it = view.NewIterator(ColumnFamily::kZScore, {std::move(lower), std::move(upper)});
  if (!it.ok()) return it.status();
  Status st = reverse ? (*it)->SeekToLast() : (*it)->SeekToFirst();
  for (; st.ok() && (*it)->Valid(); st = reverse ? (*it)->Prev() : (*it)->Next()) {
    encoding::ZScoreKey k;
    if (!encoding::DecodeZScoreKey((*it)->key(), &k)) return Status::Corruption("bad zscore key");
    auto more = fn(k.score, k.member);
    if (!more.ok()) return more.status();
    if (!*more) return Status::Ok();
  }
  return st;
}

}  // namespace

std::string EncodeRawScore(double score) {
  std::string out;
  coding::PutFixed64BE(&out, std::bit_cast<uint64_t>(score));
  return out;
}

double DecodeRawScore(std::string_view in) {
  return std::bit_cast<double>(coding::DecodeFixed64BE(in.data()));
}

Result<std::optional<double>> MemberScore(ReadView& view, const Collection& c, std::string_view member) {
  auto v = GetElement(view, c, member);
  if (!v.ok()) return v.status();
  const auto& raw = *v;
  if (!raw) return std::optional<double>();
  if (raw->size() != 8) return Status::Corruption("bad zset score");
  return std::optional<double>(DecodeRawScore(*raw));
}

Status PutMember(WriteTxn& txn, Collection& c, std::string_view member, double score,
                 std::optional<double> old) {
  score = NormalizeScore(score);
  if (old) {
    if (Status st = txn.Delete(ColumnFamily::kZScore, OrderKey(c, *old, member)); !st.ok()) return st;
  } else {
    ++c.obj.meta.size;
  }
  if (Status st = txn.Put(ColumnFamily::kSubkey, c.SubkeyFor(member), EncodeRawScore(score)); !st.ok())
    return st;
  return txn.Put(ColumnFamily::kZScore, OrderKey(c, score, member), {});
}

Status DeleteMember(WriteTxn& txn, Collection& c, std::string_view member, double score) {
  if (Status st = txn.Delete(ColumnFamily::kSubkey, c.SubkeyFor(member)); !st.ok()) return st;
  if (Status st = txn.Delete(ColumnFamily::kZScore, OrderKey(c, score, member)); !st.ok()) return st;
  --c.obj.meta.size;
  return Status::Ok();
}

Status ForEachByScore(ReadView& view, const Collection& c, bool reverse, const ScoredFn& fn) {
  if (!c.exists()) return Status::Ok();
  std::string prefix = OrderPrefix(c);
  std::string upper = encoding::PrefixUpperBound(prefix);
  return Scan(view, std::move(prefix), std::move(upper), reverse, fn);
}

Status ForEachInScoreRange(ReadView& view, const Collection& c, const ScoreRange& range, bool reverse,
                           const ScoredFn& fn) {
  if (!c.exists() || ScoreRangeEmpty(range)) return Status::Ok();
  const std::string prefix = OrderPrefix(c);
  // Every entry with score <= max sorts below PrefixUpperBound(prefix + enc(max)).
  std::string lower = prefix + encoding::EncodeScore(NormalizeScore(range.min));
  std::string upper = encoding::PrefixUpperBound(prefix + encoding::EncodeScore(NormalizeScore(range.max)));
  return Scan(view, std::move(lower), std::move(upper), reverse,
              [&](double s, std::string_view m) -> Result<bool> {
                if (!ScoreAboveMin(s, range) || !ScoreBelowMax(s, range)) return true;  // the exclusive ends
                return fn(s, m);
              });
}

Status ForEachInLexRange(ReadView& view, const Collection& c, const LexRange& range, bool reverse,
                         const ScoredFn& fn) {
  if (!c.exists() || LexRangeEmpty(range)) return Status::Ok();
  bool started = false;
  return ForEachByScore(view, c, reverse, [&](double s, std::string_view m) -> Result<bool> {
    const bool before = reverse ? !LexBelowMax(m, range) : !LexAboveMin(m, range);
    if (!started && before) return true;  // not yet at the first element inside the range
    started = true;
    const bool inside = reverse ? LexAboveMin(m, range) : LexBelowMax(m, range);
    if (!inside) return false;  // the run ends at the first element past the far bound
    return fn(s, m);
  });
}

Result<ZAddResult> ZsetAdd(WriteTxn& txn, OidAllocator& oids, uint8_t db, std::string_view key,
                           const ZAddFlags& f, const std::vector<ScoreMember>& pairs, int64_t now_ms) {
  auto c = f.xx ? LoadCollectionForWrite(txn, db, key, kZSet, now_ms)
                : OpenCollectionForWrite(txn, oids, db, key, kZSet, now_ms);
  if (!c.ok()) return c.status();
  ZAddResult r;
  if (!c->exists()) return r;  // XX on a missing key
  for (const ScoreMember& p : pairs) {
    auto old = MemberScore(txn, *c, p.member);
    if (!old.ok()) return old.status();
    const auto& prior = *old;
    double score = p.score;
    if (prior) {
      if (f.nx) continue;
      if (f.incr) {
        score += *prior;
        if (std::isnan(score)) return Status::UserError(std::string(kErrScoreNan));
      }
      if ((f.lt && score >= *prior) || (f.gt && score <= *prior)) continue;
      if (f.incr) r.incr_score = score;
      if (NormalizeScore(score) == *prior) continue;
      if (Status st = PutMember(txn, *c, p.member, score, prior); !st.ok()) return st;
      ++r.updated;
    } else {
      if (f.xx) continue;
      if (f.incr) r.incr_score = score;
      if (Status st = PutMember(txn, *c, p.member, score, std::nullopt); !st.ok()) return st;
      ++r.added;
    }
  }
  if (r.added + r.updated == 0) return r;
  if (Status st = SaveCollection(txn, *c); !st.ok()) return st;
  return r;
}

Result<int64_t> ZsetRemove(WriteTxn& txn, uint8_t db, std::string_view key,
                           const std::vector<std::string_view>& members, int64_t now_ms) {
  auto c = LoadCollectionForWrite(txn, db, key, kZSet, now_ms);
  if (!c.ok()) return c.status();
  if (!c->exists()) return int64_t{0};
  int64_t removed = 0;
  for (std::string_view m : members) {
    auto old = MemberScore(txn, *c, m);  // sees this txn's deletes: a repeated member counts once
    if (!old.ok()) return old.status();
    const auto& prior = *old;
    if (!prior) continue;
    if (Status st = DeleteMember(txn, *c, m, *prior); !st.ok()) return st;
    ++removed;
  }
  if (removed == 0) return removed;
  if (Status st = SaveCollection(txn, *c); !st.ok()) return st;
  return removed;
}

Result<int64_t> ZsetCard(ReadView& view, uint8_t db, std::string_view key, int64_t now_ms) {
  auto c = LoadCollection(view, db, key, kZSet, now_ms);
  if (!c.ok()) return c.status();
  return static_cast<int64_t>(c->size());
}

Result<std::optional<double>> ZsetScore(ReadView& view, uint8_t db, std::string_view key,
                                        std::string_view member, int64_t now_ms) {
  auto c = LoadCollection(view, db, key, kZSet, now_ms);
  if (!c.ok()) return c.status();
  return MemberScore(view, *c, member);
}

Result<std::vector<std::optional<double>>> ZsetMultiScore(ReadView& view, uint8_t db, std::string_view key,
                                                          const std::vector<std::string_view>& members,
                                                          int64_t now_ms) {
  auto c = LoadCollection(view, db, key, kZSet, now_ms);
  if (!c.ok()) return c.status();
  std::vector<std::optional<double>> out;
  out.reserve(members.size());
  for (std::string_view m : members) {
    auto s = MemberScore(view, *c, m);
    if (!s.ok()) return s.status();
    out.push_back(*s);
  }
  return out;
}

Result<int64_t> ZsetCount(ReadView& view, uint8_t db, std::string_view key, const ScoreRange& range,
                          int64_t now_ms) {
  auto c = LoadCollection(view, db, key, kZSet, now_ms);
  if (!c.ok()) return c.status();
  int64_t n = 0;
  Status st = ForEachInScoreRange(view, *c, range, false, [&](double, std::string_view) -> Result<bool> {
    ++n;
    return true;
  });
  if (!st.ok()) return st;
  return n;
}

Result<int64_t> ZsetLexCount(ReadView& view, uint8_t db, std::string_view key, const LexRange& range,
                             int64_t now_ms) {
  auto c = LoadCollection(view, db, key, kZSet, now_ms);
  if (!c.ok()) return c.status();
  int64_t n = 0;
  Status st = ForEachInLexRange(view, *c, range, false, [&](double, std::string_view) -> Result<bool> {
    ++n;
    return true;
  });
  if (!st.ok()) return st;
  return n;
}

}  // namespace tideskv
