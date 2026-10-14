#include "common/score_format.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

extern "C" {
#include "fpconv_dtoa.h"
}

namespace tideskv {

std::string FormatScore(double v) {
  if (std::isnan(v)) return "nan";
  if (std::isinf(v)) return v < 0 ? "-inf" : "inf";
  if (v == 0) return std::signbit(v) ? "-0" : "0";
  // double2ll (util.c:590): |v| <= LLONG_MAX/2 and integral.
  // The division is deliberately done in integer arithmetic (as Redis does) before converting.
  constexpr int64_t kHalfLl = std::numeric_limits<int64_t>::max() / 2;
  constexpr auto kHalf = static_cast<double>(kHalfLl);
  if (v >= -kHalf && v <= kHalf) {
    const auto ll = static_cast<int64_t>(v);
    if (static_cast<double>(ll) == v) return std::to_string(ll);
  }
  return FormatDtoa(v);
}

std::string FormatDtoa(double v) {
  char buf[32];
  const int n = fpconv_dtoa(v, buf);
  return std::string(buf, static_cast<size_t>(n));
}

std::string FormatScore17g(double v) {
  if (std::isnan(v)) return "nan";
  if (std::isinf(v)) return v < 0 ? "-inf" : "inf";
  char buf[64];
  const int n = std::snprintf(buf, sizeof(buf), "%.17Lg", static_cast<long double>(v));
  return std::string(buf, static_cast<size_t>(n));
}

bool ParseScore(std::string_view s, double* out) {
  if (s.empty() || std::isspace(static_cast<unsigned char>(s.front()))) return false;
  const std::string buf(s);  // NUL-terminated copy; an embedded NUL stops strtod early -> rejected below
  errno = 0;
  char* end = nullptr;
  const double d = std::strtod(buf.c_str(), &end);
  if (static_cast<size_t>(end - buf.c_str()) != s.size() || std::isnan(d)) return false;
  if (errno == ERANGE && (d == HUGE_VAL || d == -HUGE_VAL || std::fpclassify(d) == FP_ZERO)) return false;
  *out = d;
  return true;
}

}  // namespace tideskv
