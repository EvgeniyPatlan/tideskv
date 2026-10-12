#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"

// Longest common subsequence, ported from Redis 7.2 lcsCommand (same ranges, same order).
namespace tideskv {

struct LcsMatch {
  uint32_t a_start = 0, a_end = 0, b_start = 0, b_end = 0, len = 0;
};

struct LcsResult {
  uint32_t length = 0;
  std::string sequence;
  std::vector<LcsMatch> matches;  // from the end of the strings backwards
};

Result<LcsResult> ComputeLcs(std::string_view a, std::string_view b, bool compute_sequence,
                             int64_t min_match_len);

}  // namespace tideskv
