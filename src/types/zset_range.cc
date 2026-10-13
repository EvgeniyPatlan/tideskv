#include "types/zset_range.h"

#include <cmath>
#include <cstdlib>
#include <utility>

#include "types/errors.h"

namespace tideskv {
namespace {

// zslParseRange's item parser: "(" makes it exclusive; strtod over the rest, which must end at the
// first NUL (so "" and "(" parse as 0 and leading whitespace is allowed); NaN is rejected.
bool ParseScoreBound(std::string_view arg, double* value, bool* exclusive) {
  *exclusive = !arg.empty() && arg.front() == '(';
  const std::string rest(*exclusive ? arg.substr(1) : arg);
  char* end = nullptr;
  *value = std::strtod(rest.c_str(), &end);
  return *end == '\0' && !std::isnan(*value);
}

bool ParseLexItem(std::string_view arg, LexBound* out) {
  if (arg == "+") {
    *out = {LexBound::Kind::kPlusInf, {}, true};
  } else if (arg == "-") {
    *out = {LexBound::Kind::kMinusInf, {}, true};
  } else if (!arg.empty() && (arg.front() == '(' || arg.front() == '[')) {
    *out = {LexBound::Kind::kValue, std::string(arg.substr(1)), arg.front() == '('};
  } else {
    return false;
  }
  return true;
}

// sdscmplex: -inf < every string < +inf; strings compare as memcmp, shorter first.
int CompareLex(const LexBound& a, std::string_view b_value) {
  if (a.kind == LexBound::Kind::kMinusInf) return -1;
  if (a.kind == LexBound::Kind::kPlusInf) return 1;
  const int c = std::string_view(a.value).compare(b_value);
  return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

int CompareBounds(const LexBound& a, const LexBound& b) {
  if (a.kind == b.kind && a.kind != LexBound::Kind::kValue) return 0;
  if (a.kind == LexBound::Kind::kMinusInf || b.kind == LexBound::Kind::kPlusInf) return -1;
  if (a.kind == LexBound::Kind::kPlusInf || b.kind == LexBound::Kind::kMinusInf) return 1;
  return CompareLex(a, b.value);
}

}  // namespace

Status ParseScoreRange(std::string_view min, std::string_view max, ScoreRange* out) {
  ScoreRange r;
  if (!ParseScoreBound(min, &r.min, &r.minex) || !ParseScoreBound(max, &r.max, &r.maxex))
    return Status::UserError(std::string(kErrMinMaxNotFloat));
  *out = r;
  return Status::Ok();
}

bool ScoreAboveMin(double v, const ScoreRange& r) { return r.minex ? v > r.min : v >= r.min; }
bool ScoreBelowMax(double v, const ScoreRange& r) { return r.maxex ? v < r.max : v <= r.max; }
bool ScoreRangeEmpty(const ScoreRange& r) {
  return r.min > r.max || (r.min == r.max && (r.minex || r.maxex));
}

Status ParseLexRange(std::string_view min, std::string_view max, LexRange* out) {
  LexRange r;
  if (!ParseLexItem(min, &r.min) || !ParseLexItem(max, &r.max))
    return Status::UserError(std::string(kErrMinMaxNotString));
  *out = std::move(r);
  return Status::Ok();
}

bool LexAboveMin(std::string_view member, const LexRange& r) {
  const int c = -CompareLex(r.min, member);  // compare(member, min)
  return r.min.exclusive ? c > 0 : c >= 0;
}

bool LexBelowMax(std::string_view member, const LexRange& r) {
  const int c = -CompareLex(r.max, member);  // compare(member, max)
  return r.max.exclusive ? c < 0 : c <= 0;
}

bool LexRangeEmpty(const LexRange& r) {
  const int c = CompareBounds(r.min, r.max);
  return c > 0 || (c == 0 && (r.min.exclusive || r.max.exclusive));
}

}  // namespace tideskv
