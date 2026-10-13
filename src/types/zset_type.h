#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "storage/storage.h"
#include "types/collection.h"
#include "types/oid_allocator.h"
#include "types/zset_range.h"

// Sorted-set logic (spec §4.4, M4 plan F2) on the collection layer:
//   subkey [prefix][member] -> raw score (8 bytes BE), zscore [prefix][EncodeScore(score)][member] -> "".
// Stored scores are normalised (-0 -> 0). WRONGTYPE for a live key of another type.
namespace tideskv {

struct ZAddFlags {
  bool nx = false, xx = false, gt = false, lt = false, ch = false, incr = false;
};
struct ScoreMember {
  double score = 0;
  std::string member;
};
struct ZAddResult {
  int64_t added = 0;
  int64_t updated = 0;               // score actually changed
  std::optional<double> incr_score;  // INCR: the new score, nullopt when the pair was a no-op
};

// ZADD/ZINCRBY (zsetAdd, redis72-zset.md §1). Flag combinations and pair counts are validated by the
// caller. XX on a missing key creates nothing. INCR reaching NaN: kErrScoreNan.
Result<ZAddResult> ZsetAdd(WriteTxn& txn, OidAllocator& oids, uint8_t db, std::string_view key,
                           const ZAddFlags& flags, const std::vector<ScoreMember>& pairs, int64_t now_ms);
// ZREM: the number of distinct present members removed; the key goes with its last member.
Result<int64_t> ZsetRemove(WriteTxn& txn, uint8_t db, std::string_view key,
                           const std::vector<std::string_view>& members, int64_t now_ms);
Result<int64_t> ZsetCard(ReadView& view, uint8_t db, std::string_view key, int64_t now_ms);
Result<std::optional<double>> ZsetScore(ReadView& view, uint8_t db, std::string_view key,
                                        std::string_view member, int64_t now_ms);
// ZMSCORE: one entry per member (all nullopt for a missing key).
Result<std::vector<std::optional<double>>> ZsetMultiScore(ReadView& view, uint8_t db, std::string_view key,
                                                          const std::vector<std::string_view>& members,
                                                          int64_t now_ms);
Result<int64_t> ZsetCount(ReadView& view, uint8_t db, std::string_view key, const ScoreRange& range,
                          int64_t now_ms);
Result<int64_t> ZsetLexCount(ReadView& view, uint8_t db, std::string_view key, const LexRange& range,
                             int64_t now_ms);

// Building blocks (zset_query, zset_algebra, blocking pops).
std::string EncodeRawScore(double score);
double DecodeRawScore(std::string_view eight_bytes);
Result<std::optional<double>> MemberScore(ReadView& view, const Collection& c, std::string_view member);
// Writes member with `score` (normalised here); `old` is its current score, if any (its order entry is
// replaced). Adds to c's size for a new member. The caller saves the collection.
Status PutMember(WriteTxn& txn, Collection& c, std::string_view member, double score,
                 std::optional<double> old);
// Deletes member (whose stored score is `score`) and decrements c's size. The caller saves.
Status DeleteMember(WriteTxn& txn, Collection& c, std::string_view member, double score);

using ScoredFn = std::function<Result<bool>(double score, std::string_view member)>;  // false: stop
// Every member in (score, member) order, or the reverse.
Status ForEachByScore(ReadView& view, const Collection& c, bool reverse, const ScoredFn& fn);
// Members whose score is in `range`, ascending (or descending with reverse); an empty range visits nothing.
Status ForEachInScoreRange(ReadView& view, const Collection& c, const ScoreRange& range, bool reverse,
                           const ScoredFn& fn);
// Redis's linear lex semantic (redis72-zset.md §0.6): from the first member >= min in rank order, while
// <= max (reverse: from the last member <= max, while >= min).
Status ForEachInLexRange(ReadView& view, const Collection& c, const LexRange& range, bool reverse,
                         const ScoredFn& fn);

}  // namespace tideskv
