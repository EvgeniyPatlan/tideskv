#pragma once
#include <string>
#include <string_view>

// Order-preserving 8-byte encoding of an IEEE-754 double (spec §4.1):
// positive → flip sign bit; negative → invert all bits. -0.0 is normalised to +0.0.
namespace tideskv::encoding {

void AppendScore(std::string* dst, double score);  // NaN is a caller bug (asserted)
std::string EncodeScore(double score);
double DecodeScore(std::string_view eight_bytes);   // requires size() >= 8

}  // namespace tideskv::encoding
