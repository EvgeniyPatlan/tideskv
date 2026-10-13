#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "storage/storage.h"
#include "types/zset_type.h"

// ZUNION/ZINTER/ZDIFF (+STORE) and ZINTERCARD (redis72-zset.md §8). Inputs are materialised in memory, as
// Redis does.
namespace tideskv {

enum class ZsetOp : uint8_t { kUnion, kInter, kDiff };
enum class Aggregate : uint8_t { kSum, kMin, kMax };

using ZsetSource = std::map<std::string, double, std::less<>>;  // member -> score

// One source per key, in order: a missing key is empty, a set scores 1.0 per member, a zset its scores;
// any other live type is WRONGTYPE (checked in key order before anything else is computed).
Result<std::vector<ZsetSource>> LoadZsetSources(ReadView& view, uint8_t db,
                                                const std::vector<std::string_view>& keys, int64_t now_ms);
// weights has one entry per source (ignored for kDiff). first_key_repeated: a later key names the first key
// (Redis's ZDIFF shortcut: empty result). The result is sorted by (score, member bytes).
std::vector<ScoreMember> CombineZsets(ZsetOp op, const std::vector<ZsetSource>& sources,
                                      const std::vector<double>& weights, Aggregate aggregate,
                                      bool first_key_repeated);
// ZINTERCARD: members present in every source, stopping at `limit` (0 = no limit).
uint64_t ZsetInterCardinality(const std::vector<ZsetSource>& sources, uint64_t limit);

}  // namespace tideskv
