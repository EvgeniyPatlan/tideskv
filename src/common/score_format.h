#pragma once
#include <string>
#include <string_view>

// Sorted-set score text, exactly as Redis 7.2.5 (redis72-zset.md §0.4, §0.8, §0.9).
namespace tideskv {

// d2string (util.c:623): nan, inf, -inf, 0, -0; integers within +-2^62 as integers; otherwise fpconv_dtoa.
std::string FormatScore(double v);
// ld2string(LD_STR_AUTO): "%.17Lg" (the ZSCAN reply of a large zset).
std::string FormatScore17g(double v);
// string2d (util.c:574): strtod over every byte; empty, leading space, trailing junk, overflow, underflow to
// zero and NaN are rejected. "-0" parses as -0.0.
bool ParseScore(std::string_view s, double* out);
// -0.0 -> +0.0; everything else unchanged (M4 plan F2).
inline double NormalizeScore(double v) { return v == 0.0 ? 0.0 : v; }

}  // namespace tideskv
