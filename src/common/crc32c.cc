#include "common/crc32c.h"

#include <array>
#include <cstddef>
#include <cstring>

#if defined(__SSE4_2__)
#include <nmmintrin.h>
#endif

namespace tideskv {
namespace {

constexpr uint32_t kPoly = 0x82F63B78U;

using SliceTables = std::array<std::array<uint32_t, 256>, 8>;

constexpr SliceTables MakeTables() {
  SliceTables t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1U) ? kPoly : 0U);
    t[0][i] = crc;
  }
  for (uint32_t i = 0; i < 256; ++i) {
    for (size_t s = 1; s < 8; ++s) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFFU];
  }
  return t;
}

constexpr SliceTables kTables = MakeTables();

#if defined(__SSE4_2__)
uint32_t Update(uint32_t crc, const unsigned char* p, size_t n) {
  uint64_t c = crc;
  for (; n >= 8; n -= 8, p += 8) {
    uint64_t word = 0;
    std::memcpy(&word, p, 8);
    c = _mm_crc32_u64(c, word);
  }
  auto c32 = static_cast<uint32_t>(c);
  for (; n > 0; --n, ++p) c32 = _mm_crc32_u8(c32, *p);
  return c32;
}
#else
uint32_t Update(uint32_t crc, const unsigned char* p, size_t n) {
  for (; n >= 8; n -= 8, p += 8) {
    uint32_t lo = 0, hi = 0;
    std::memcpy(&lo, p, 4);
    std::memcpy(&hi, p + 4, 4);
    // The tables assume a little-endian load; swap on big-endian hosts.
    if constexpr (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) {
      lo = __builtin_bswap32(lo);
      hi = __builtin_bswap32(hi);
    }
    lo ^= crc;
    crc = kTables[7][lo & 0xFFU] ^ kTables[6][(lo >> 8) & 0xFFU] ^ kTables[5][(lo >> 16) & 0xFFU] ^
          kTables[4][lo >> 24] ^ kTables[3][hi & 0xFFU] ^ kTables[2][(hi >> 8) & 0xFFU] ^
          kTables[1][(hi >> 16) & 0xFFU] ^ kTables[0][hi >> 24];
  }
  for (; n > 0; --n, ++p) crc = (crc >> 8) ^ kTables[0][(crc ^ *p) & 0xFFU];
  return crc;
}
#endif

}  // namespace

uint32_t Crc32c(std::string_view data, uint32_t seed) {
  return ~Update(~seed, reinterpret_cast<const unsigned char*>(data.data()), data.size());
}

}  // namespace tideskv
