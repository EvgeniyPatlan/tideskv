#include "common/crc32c.h"

#include <gtest/gtest.h>

#include <string>

namespace tideskv {
namespace {

// Bitwise reference: reflected Castagnoli polynomial, pre- and post-inverted.
uint32_t ReferenceCrc32c(const char* p, size_t n, uint32_t seed = 0) {
  uint32_t crc = ~seed;
  for (size_t i = 0; i < n; ++i) {
    crc ^= static_cast<unsigned char>(p[i]);
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1U) ? 0x82F63B78U : 0U);
  }
  return ~crc;
}

TEST(Crc32c, KnownVectors) {
  EXPECT_EQ(Crc32c(""), 0x00000000u);
  EXPECT_EQ(Crc32c("123456789"), 0xE3069283u);              // the CRC-32C check value
  EXPECT_EQ(Crc32c(std::string(32, '\0')), 0x8A9136AAu);    // RFC 3720 B.4
  EXPECT_EQ(Crc32c(std::string(32, '\xff')), 0x62A8AB43u);  // RFC 3720 B.4
}

TEST(Crc32c, SeedChainsLikeOneCall) {
  const std::string a = "hello ", b = "world";
  EXPECT_EQ(Crc32c(b, Crc32c(a)), Crc32c(a + b));
}

TEST(Crc32c, UnalignedAndLongInputsMatchBytewise) {
  std::string buf(300 + 8, '\0');
  uint32_t x = 12345;
  for (char& c : buf) {
    x = x * 1664525U + 1013904223U;
    c = static_cast<char>(x >> 24);
  }
  for (size_t offset = 0; offset < 8; ++offset) {
    for (size_t len = 0; len <= 300; ++len) {
      const std::string_view v(buf.data() + offset, len);
      ASSERT_EQ(Crc32c(v), ReferenceCrc32c(v.data(), len)) << "offset " << offset << " len " << len;
      ASSERT_EQ(Crc32c(v, 0xDEADBEEFu), ReferenceCrc32c(v.data(), len, 0xDEADBEEFu));
    }
  }
}

}  // namespace
}  // namespace tideskv
