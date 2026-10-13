#pragma once
#include <optional>
#include <vector>

#include "commands/registry.h"
#include "types/zset_pop.h"
#include "types/zset_type.h"

namespace tideskv {

// Members, or with scores: RESP2 a flat [m1, s1, m2, s2 ...] (scores as bulk strings), RESP3
// [[m1, s1], [m2, s2] ...] with double scores (redis72-zset.md §0.11).
void ReplyScored(ExecContext& ctx, const std::vector<ScoreMember>& items, bool with_scores);

// ZMPOP/BZMPOP: [key, [[member, score], ...]] in both protocols; a null array for nullopt.
void ReplyZPopped(ExecContext& ctx, const std::optional<ZPoppedFrom>& popped);

}  // namespace tideskv
