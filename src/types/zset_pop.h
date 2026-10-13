#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "storage/storage.h"
#include "types/zset_query.h"
#include "types/zset_type.h"

// Sorted-set removals and random picks (redis72-zset.md §2, §4, §6).
namespace tideskv {

// Redis's zset-max-listpack-entries / -value defaults: below both, Redis keeps a "small" zset whose ZSCAN
// returns everything at once in score order (M4 plan F2).
inline constexpr size_t kZsetSmallMax = 128;
inline constexpr size_t kZsetSmallValueMax = 64;

// ZREMRANGEBYRANK/SCORE/LEX (spec.offset/count unused): the number removed; the key goes with its last
// member.
Result<int64_t> ZsetRemoveRange(WriteTxn& txn, uint8_t db, std::string_view key, const RangeSpec& spec,
                                int64_t now_ms);
// ZPOPMIN (max = false) / ZPOPMAX: up to `count` members in pop order; nullopt for a missing key.
Result<std::optional<std::vector<ScoreMember>>> ZsetPop(WriteTxn& txn, uint8_t db, std::string_view key,
                                                        bool max, uint64_t count, int64_t now_ms);

struct ZPoppedFrom {
  std::string key;  // as given in the arguments
  std::vector<ScoreMember> items;
};
// ZMPOP / BZPOPMIN / BZPOPMAX / BZMPOP core: the first existing key decides (WRONGTYPE stops the scan,
// later keys are not examined); nullopt when no key exists.
Result<std::optional<ZPoppedFrom>> ZsetPopFirst(WriteTxn& txn, uint8_t db,
                                                const std::vector<std::string_view>& keys, bool max,
                                                uint64_t count, int64_t now_ms);

// ZRANDMEMBER key: nullopt for a missing key.
Result<std::optional<std::string>> ZsetRandomMember(ReadView& view, uint8_t db, std::string_view key,
                                                    std::mt19937_64& rng, int64_t now_ms);
// ZRANDMEMBER key count (count != INT64_MIN): count >= size is the whole set in descending order; other
// positive counts are distinct random members; a negative count is |count| picks with repeats.
Result<std::vector<ScoreMember>> ZsetRandomMembers(ReadView& view, uint8_t db, std::string_view key,
                                                   int64_t count, std::mt19937_64& rng, int64_t now_ms);

}  // namespace tideskv
