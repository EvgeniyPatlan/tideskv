#pragma once
#include <cstdint>
#include <string>
#include <string_view>

#include "common/status.h"

// ZRANGEBYSCORE/ZCOUNT/... score ranges (zslParseRange) and ZRANGEBYLEX/... lex ranges
// (zslParseLexRange), with Redis 7.2.5's comparison rules (redis72-zset.md §0.5-0.6).
namespace tideskv {

struct ScoreRange {
  double min = 0, max = 0;
  bool minex = false, maxex = false;  // exclusive bounds ("(")
};

// Both bounds are parsed before any key lookup; failure: kErrMinMaxNotFloat.
Status ParseScoreRange(std::string_view min, std::string_view max, ScoreRange* out);
bool ScoreAboveMin(double v, const ScoreRange& r);  // v >= min (v > min if exclusive)
bool ScoreBelowMax(double v, const ScoreRange& r);  // v <= max (v < max if exclusive)
inline bool ScoreInRange(double v, const ScoreRange& r) { return ScoreAboveMin(v, r) && ScoreBelowMax(v, r); }
bool ScoreRangeEmpty(const ScoreRange& r);  // min > max, or min == max with an exclusive bound

struct LexBound {
  enum class Kind : uint8_t { kMinusInf, kPlusInf, kValue } kind = Kind::kValue;
  std::string value;
  bool exclusive = false;
};
struct LexRange {
  LexBound min, max;
};

// Items: "-", "+", "(bytes" or "[bytes"; anything else is kErrMinMaxNotString.
Status ParseLexRange(std::string_view min, std::string_view max, LexRange* out);
bool LexAboveMin(std::string_view member, const LexRange& r);
bool LexBelowMax(std::string_view member, const LexRange& r);
bool LexRangeEmpty(const LexRange& r);

}  // namespace tideskv
