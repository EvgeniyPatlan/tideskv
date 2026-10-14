#pragma once
#include <cstdint>
#include <string_view>

namespace tideskv {

// CRC-32C (Castagnoli, reflected polynomial 0x82F63B78) as in RFC 3720. `seed` is a previous
// result, so Crc32c(b, Crc32c(a)) == Crc32c(a + b).
uint32_t Crc32c(std::string_view data, uint32_t seed = 0);

}  // namespace tideskv
