#pragma once
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "storage/storage.h"
#include "types/oid_allocator.h"
#include "types/zset_range.h"
#include "types/zset_type.h"

// ZRANGE-family queries, ranks and stores (redis72-zset.md §2-3). Rank and index operations iterate:
// O(start + count) and O(rank) (M4 plan F2).
namespace tideskv {

enum class RangeBy : uint8_t { kRank, kScore, kLex };

struct RangeSpec {
  RangeBy by = RangeBy::kRank;
  bool reverse = false;
  int64_t start = 0, stop = 0;  // kRank, Redis index rules; with reverse, index 0 is the highest
  ScoreRange score;             // kScore
  LexRange lex;                 // kLex
  int64_t offset = 0;           // LIMIT (score/lex): a negative offset gives nothing
  int64_t count = -1;           // negative: all
};

// (member, score) pairs in result order; empty for a missing key.
Result<std::vector<ScoreMember>> ZsetRange(ReadView& view, uint8_t db, std::string_view key,
                                           const RangeSpec& spec, int64_t now_ms);
// ZRANGESTORE: StoreZset(dst, range of src); returns the number stored.
Result<int64_t> ZsetRangeStore(WriteTxn& txn, OidAllocator& oids, uint8_t db, std::string_view dst,
                               std::string_view src, const RangeSpec& spec, int64_t now_ms);

struct RankResult {
  int64_t rank = 0;
  double score = 0;
};
Result<std::optional<RankResult>> ZsetRank(ReadView& view, uint8_t db, std::string_view key,
                                           std::string_view member, bool reverse, int64_t now_ms);

// Replaces dst (any type; its TTL goes) by a new zset of `items`; empty items just delete dst.
// Duplicate members keep the last score.
Status StoreZset(WriteTxn& txn, OidAllocator& oids, uint8_t db, std::string_view dst,
                 const std::vector<ScoreMember>& items, int64_t now_ms);

}  // namespace tideskv
