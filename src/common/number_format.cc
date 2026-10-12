#include "common/number_format.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tideskv {
namespace {

constexpr size_t kMaxLongDoubleChars = size_t{5} * 1024;  // Redis MAX_LONG_DOUBLE_CHARS

}  // namespace

bool ParseLongDouble(std::string_view s, long double* out) {
  if (s.empty() || s.size() >= kMaxLongDoubleChars || std::memchr(s.data(), '\0', s.size()) != nullptr) {
    return false;
  }
  char buf[kMaxLongDoubleChars];
  std::memcpy(buf, s.data(), s.size());
  buf[s.size()] = '\0';
  if (std::isspace(static_cast<unsigned char>(buf[0]))) return false;
  errno = 0;
  char* end = nullptr;
  const long double v = std::strtold(buf, &end);
  if (*end != '\0' || std::isnan(v) || errno == EINVAL) return false;
  if (errno == ERANGE && (v == HUGE_VALL || v == -HUGE_VALL || std::fpclassify(v) == FP_ZERO)) return false;
  *out = v;
  return true;
}

std::string FormatLongDouble(long double v) {
  char buf[kMaxLongDoubleChars];
  int len = std::snprintf(buf, sizeof(buf), "%.17Lf", v);
  if (len < 0 || static_cast<size_t>(len) >= sizeof(buf)) return "0";  // unreachable for finite values
  if (std::strchr(buf, '.') != nullptr) {
    while (buf[len - 1] == '0') --len;
    if (buf[len - 1] == '.') --len;
  }
  if (len == 2 && buf[0] == '-' && buf[1] == '0') return "0";
  return std::string(buf, static_cast<size_t>(len));
}

}  // namespace tideskv
