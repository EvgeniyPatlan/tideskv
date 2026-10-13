#include "script/sha1hex.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

extern "C" {
#include <sha1.h>
}

namespace tideskv {

std::string Sha1Hex(std::string_view data) {
  SHA1_CTX ctx;
  SHA1Init(&ctx);
  // SHA1Update takes a uint32_t length: feed large inputs in chunks.
  constexpr size_t kChunk = size_t{1} << 30;
  for (size_t off = 0; off < data.size(); off += kChunk) {
    const size_t n = std::min(kChunk, data.size() - off);
    SHA1Update(&ctx, reinterpret_cast<const unsigned char*>(data.data() + off), static_cast<uint32_t>(n));
  }
  unsigned char digest[20];
  SHA1Final(digest, &ctx);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(40, '0');
  for (int i = 0; i < 20; ++i) {
    out[2 * i] = kHex[digest[i] >> 4];
    out[2 * i + 1] = kHex[digest[i] & 0xf];
  }
  return out;
}

}  // namespace tideskv
