#include "common/glob.h"

#include <utility>

namespace tideskv {
namespace {

unsigned char Fold(char c, bool nocase) {
  auto u = static_cast<unsigned char>(c);
  return (nocase && u >= 'A' && u <= 'Z') ? static_cast<unsigned char>(u - 'A' + 'a') : u;
}

struct ClassResult {
  bool matched;
  size_t next;  // pattern index just past the closing ']'
};

// pattern[p] == '['. Matches ch against the class; mirrors Redis stringmatchlen.
ClassResult MatchClass(std::string_view pat, size_t p, char ch, bool nocase) {
  size_t i = p + 1;
  bool negate = false;
  if (i < pat.size() && pat[i] == '^') {
    negate = true;
    ++i;
  }
  const unsigned char c = Fold(ch, nocase);
  bool matched = false;
  while (i < pat.size() && pat[i] != ']') {
    if (pat[i] == '\\' && i + 1 < pat.size()) {
      matched |= Fold(pat[i + 1], nocase) == c;
      i += 2;
    } else if (i + 2 < pat.size() && pat[i + 1] == '-') {
      unsigned char lo = Fold(pat[i], nocase);
      unsigned char hi = Fold(pat[i + 2], nocase);
      if (lo > hi) std::swap(lo, hi);
      matched |= (c >= lo && c <= hi);
      i += 3;
    } else {
      matched |= Fold(pat[i], nocase) == c;
      ++i;
    }
  }
  const size_t next = i < pat.size() ? i + 1 : pat.size();
  return {negate ? !matched : matched, next};
}

// Tries to match one pattern element at pat[p] against ch. Returns next p, or npos.
size_t MatchOne(std::string_view pat, size_t p, char ch, bool nocase) {
  constexpr size_t kNoMatch = std::string_view::npos;
  switch (pat[p]) {
    case '?':
      return p + 1;
    case '[': {
      const ClassResult r = MatchClass(pat, p, ch, nocase);
      return r.matched ? r.next : kNoMatch;
    }
    case '\\':
      if (p + 1 < pat.size()) return Fold(pat[p + 1], nocase) == Fold(ch, nocase) ? p + 2 : kNoMatch;
      return Fold('\\', nocase) == Fold(ch, nocase) ? p + 1 : kNoMatch;
    default:
      return Fold(pat[p], nocase) == Fold(ch, nocase) ? p + 1 : kNoMatch;
  }
}

}  // namespace

bool GlobMatch(std::string_view pat, std::string_view str, bool nocase) {
  constexpr size_t kNone = std::string_view::npos;
  size_t p = 0, s = 0;
  size_t star_p = kNone, star_s = 0;
  while (s < str.size()) {
    if (p < pat.size() && pat[p] == '*') {
      while (p < pat.size() && pat[p] == '*') ++p;
      star_p = p;
      star_s = s;
      continue;
    }
    const size_t next = p < pat.size() ? MatchOne(pat, p, str[s], nocase) : kNone;
    if (next != kNone) {
      p = next;
      ++s;
    } else if (star_p != kNone) {
      p = star_p;
      s = ++star_s;
    } else {
      return false;
    }
  }
  while (p < pat.size() && pat[p] == '*') ++p;
  return p == pat.size();
}

}  // namespace tideskv
