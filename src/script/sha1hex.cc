#include "script/sha1hex.h"

#include <cstddef>

#include "common/sha1.h"

namespace tideskv {

std::string Sha1Hex(std::string_view data) {
  const Sha1Digest digest = Sha1(data);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(40, '0');
  for (size_t i = 0; i < digest.size(); ++i) {
    out[2 * i] = kHex[digest[i] >> 4];
    out[2 * i + 1] = kHex[digest[i] & 0xf];
  }
  return out;
}

}  // namespace tideskv
