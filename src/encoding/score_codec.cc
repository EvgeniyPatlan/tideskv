#include "encoding/score_codec.h"

#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>

#include "common/coding.h"

namespace tideskv::encoding {

void AppendScore(std::string* dst, double score) {
  assert(!std::isnan(score));
  if (score == 0.0) score = 0.0;  // collapse -0.0
  auto bits = std::bit_cast<uint64_t>(score);
  bits = (bits >> 63) ? ~bits : (bits ^ (1ULL << 63));
  coding::PutFixed64BE(dst, bits);
}

std::string EncodeScore(double score) {
  std::string out;
  AppendScore(&out, score);
  return out;
}

double DecodeScore(std::string_view in) {
  assert(in.size() >= 8);
  uint64_t bits = coding::DecodeFixed64BE(in.data());
  bits = (bits >> 63) ? (bits ^ (1ULL << 63)) : ~bits;
  return std::bit_cast<double>(bits);
}

}  // namespace tideskv::encoding
