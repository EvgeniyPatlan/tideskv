#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace tideskv {

using Sha1Digest = std::array<uint8_t, 20>;

// SHA1 of `data` as 20 raw bytes (Redis src/sha1.c, vendored in third_party/sha1).
Sha1Digest Sha1(std::string_view data);

}  // namespace tideskv
