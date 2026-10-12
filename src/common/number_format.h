#pragma once
#include <string>
#include <string_view>

namespace tideskv {

// Redis 7.2 string2ld: strtold over the whole input; rejects leading space, trailing
// junk, NaN, out-of-range values and inputs of 5120+ bytes. Accepts "inf".
bool ParseLongDouble(std::string_view s, long double* out);
// Redis 7.2 ld2string(LD_STR_HUMAN): "%.17Lf" with trailing zeros removed; "-0" -> "0".
std::string FormatLongDouble(long double v);

}  // namespace tideskv
