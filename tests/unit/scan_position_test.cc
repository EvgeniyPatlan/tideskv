#include "types/scan_position.h"

#include <gtest/gtest.h>

#include <string>

namespace tideskv {
namespace {

// Reference SipHash-2-4 vectors (Aumasson & Bernstein, vectors.h / paper appendix A):
// key 00..0f, message 00..(n-1).
std::string Message(size_t n) {
  std::string m;
  for (size_t i = 0; i < n; ++i) m.push_back(static_cast<char>(i));
  return m;
}

SipHashKey ReferenceKey() {
  SipHashKey k{};
  for (size_t i = 0; i < k.size(); ++i) k[i] = static_cast<uint8_t>(i);
  return k;
}

TEST(SipHash24, MatchesReferenceVectors) {
  const SipHashKey key = ReferenceKey();
  EXPECT_EQ(SipHash24(key, Message(0)), 0x726fdb47dd0e0e31ULL);
  EXPECT_EQ(SipHash24(key, Message(1)), 0x74f839c593dc67fdULL);
  EXPECT_EQ(SipHash24(key, Message(8)), 0x93f5f5799a932462ULL);
  EXPECT_EQ(SipHash24(key, Message(15)), 0xa129ca6149be45e5ULL);  // the paper's worked example
}

TEST(SipHash24, PositionDigestIsStableWithinTheProcess) {
  const std::string a(1 << 20, 'a');
  EXPECT_EQ(PositionDigest(a + "1"), PositionDigest(a + "1"));
  EXPECT_NE(PositionDigest(a + "1"), PositionDigest(a + "2"));
}

}  // namespace
}  // namespace tideskv
