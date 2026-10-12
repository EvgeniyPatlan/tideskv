#include "types/scan_position.h"

namespace tideskv {

uint64_t PositionDigest(std::string_view s) {
  constexpr uint64_t kOffsetBasis = 14695981039346656037ULL;
  constexpr uint64_t kPrime = 1099511628211ULL;
  uint64_t h = kOffsetBasis;
  for (const char c : s) {
    h ^= static_cast<unsigned char>(c);
    h *= kPrime;
  }
  return h;
}

}  // namespace tideskv
