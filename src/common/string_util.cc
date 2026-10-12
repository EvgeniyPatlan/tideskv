#include "common/string_util.h"

#include <algorithm>
#include <charconv>
#include <limits>

namespace tideskv {
namespace {

char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// Check if integer string is canonical (no leading zeros, except "0" itself; no "-0").
// After optional '-', first digit must be 1-9 unless entire string is exactly "0".
bool IsCanonicalInteger(std::string_view s) {
  if (s.empty()) return false;
  size_t pos = 0;
  if (s[0] == '-') {
    if (s.size() == 1) return false;  // Just "-"
    ++pos;
  }
  if (s[pos] == '0') {
    // "0" is canonical (and only "0", not "-0" or "00" or "007")
    // So the entire string must be exactly "0" (no minus, just the single char)
    return s.size() == 1 && s[0] == '0';
  }
  return true;
}

template <typename T>
bool ParseInteger(std::string_view s, T* out) {
  if (s.empty() || !IsCanonicalInteger(s)) return false;
  T value{};
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
  if (ec != std::errc{} || ptr != s.data() + s.size()) return false;
  *out = value;
  return true;
}

}  // namespace

bool ParseInt64(std::string_view s, int64_t* out) { return ParseInteger(s, out); }

bool ParseUint64(std::string_view s, uint64_t* out) {
  if (!s.empty() && s[0] == '-') return false;
  return ParseInteger(s, out);
}

bool ParseMemory(std::string_view s, uint64_t* out) {
  const size_t digits = static_cast<size_t>(
      std::find_if(s.begin(), s.end(), [](char c) { return c < '0' || c > '9'; }) - s.begin());
  if (digits == 0) return false;
  uint64_t number = 0;
  if (!ParseUint64(s.substr(0, digits), &number)) return false;
  const std::string unit = ToLower(s.substr(digits));
  uint64_t mul = 0;
  if (unit.empty())
    mul = 1;
  else if (unit == "k")
    mul = 1000;
  else if (unit == "kb")
    mul = 1024;
  else if (unit == "m")
    mul = 1000ULL * 1000;
  else if (unit == "mb")
    mul = 1024ULL * 1024;
  else if (unit == "g")
    mul = 1000ULL * 1000 * 1000;
  else if (unit == "gb")
    mul = 1024ULL * 1024 * 1024;
  else
    return false;
  if (number > std::numeric_limits<uint64_t>::max() / mul) return false;
  *out = number * mul;
  return true;
}

std::string ToLower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), LowerAscii);
  return out;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
                                            [](char x, char y) { return LowerAscii(x) == LowerAscii(y); });
}

bool ConstantTimeEquals(std::string_view a, std::string_view b) {
  unsigned char diff = a.size() == b.size() ? 0 : 1;
  const size_t n = std::max(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    const auto ca = static_cast<unsigned char>(i < a.size() ? a[i] : 0);
    const auto cb = static_cast<unsigned char>(i < b.size() ? b[i] : 0);
    diff = static_cast<unsigned char>(diff | (ca ^ cb));
  }
  return diff == 0;
}

}  // namespace tideskv
