#include "common/sha1.h"

#include <algorithm>
#include <cstddef>

extern "C" {
#include <sha1.h>
}

namespace tideskv {

Sha1Digest Sha1(std::string_view data) {
  SHA1_CTX ctx;
  SHA1Init(&ctx);
  // SHA1Update takes a uint32_t length: feed large inputs in chunks.
  constexpr size_t kChunk = size_t{1} << 30;
  for (size_t off = 0; off < data.size(); off += kChunk) {
    const size_t n = std::min(kChunk, data.size() - off);
    SHA1Update(&ctx, reinterpret_cast<const unsigned char*>(data.data() + off), static_cast<uint32_t>(n));
  }
  Sha1Digest digest{};
  SHA1Final(digest.data(), &ctx);
  return digest;
}

}  // namespace tideskv
