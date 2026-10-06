#include "common/coding.h"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

namespace tideskv::coding {
namespace {

TEST(Coding, Fixed64IsBigEndian) {
  std::string s;
  PutFixed64BE(&s, 0x0102030405060708ULL);
  ASSERT_EQ(s.size(), 8u);
  EXPECT_EQ(s, std::string("\x01\x02\x03\x04\x05\x06\x07\x08", 8));
}

TEST(Coding, Fixed16HandlesHighBytes) {
  std::string s;
  PutFixed16BE(&s, 0xFF80);
  EXPECT_EQ(DecodeFixed16BE(s.data()), 0xFF80);
}

TEST(Coding, GetFixedRejectsShortInput) {
  std::string_view in("\x01\x02", 2);
  uint64_t v = 0;
  EXPECT_FALSE(GetFixed64BE(&in, &v));
  EXPECT_EQ(in.size(), 2u);  // nothing consumed on failure
}

TEST(Coding, VarintRejectsTruncated) {
  std::string s;
  PutVarint64(&s, 300);
  std::string_view in(s.data(), s.size() - 1);
  uint64_t v = 0;
  EXPECT_FALSE(GetVarint64(&in, &v));
}

TEST(Coding, VarintRejectsOverlong) {
  std::string s(11, '\x80');
  std::string_view in(s);
  uint64_t v = 0;
  EXPECT_FALSE(GetVarint64(&in, &v));
}

TEST(Coding, VarintRejectsTenContinuationBytesPlus11thByte) {
  // 10 continuation bytes (0x80) followed by terminating byte (0x00) — 11 bytes total
  std::string s(10, '\x80');
  s.push_back('\x00');
  std::string_view in(s);
  uint64_t v = 0;
  EXPECT_FALSE(GetVarint64(&in, &v));
}

TEST(Coding, VarintRejectsOverflowBitsIn10thByte) {
  // 10th byte with overflow bits set: FF FF FF FF FF FF FF FF FF 7F
  std::string s("\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x7F", 10);
  std::string_view in(s);
  uint64_t v = 0;
  EXPECT_FALSE(GetVarint64(&in, &v));
}

TEST(Coding, VarintRoundTripsUINT64MAX) {
  std::string s;
  PutVarint64(&s, UINT64_MAX);
  // UINT64_MAX should encode to exactly 10 bytes
  ASSERT_EQ(s.size(), 10u);
  std::string_view in(s);
  uint64_t out = 0;
  EXPECT_TRUE(GetVarint64(&in, &out));
  EXPECT_EQ(out, UINT64_MAX);
  EXPECT_TRUE(in.empty());
}

RC_GTEST_PROP(CodingProp, Fixed64RoundTrips, (uint64_t v)) {
  std::string s;
  PutFixed64BE(&s, v);
  std::string_view in(s);
  uint64_t out = 0;
  RC_ASSERT(GetFixed64BE(&in, &out));
  RC_ASSERT(out == v);
  RC_ASSERT(in.empty());
}

// The property every key layout depends on: byte order == numeric order.
RC_GTEST_PROP(CodingProp, Fixed64PreservesOrder, (uint64_t a, uint64_t b)) {
  std::string ea, eb;
  PutFixed64BE(&ea, a);
  PutFixed64BE(&eb, b);
  RC_ASSERT((a < b) == (ea < eb));
}

RC_GTEST_PROP(CodingProp, Fixed16PreservesOrder, (uint16_t a, uint16_t b)) {
  std::string ea, eb;
  PutFixed16BE(&ea, a);
  PutFixed16BE(&eb, b);
  RC_ASSERT((a < b) == (ea < eb));
}

RC_GTEST_PROP(CodingProp, VarintRoundTrips, (uint64_t v)) {
  std::string s;
  PutVarint64(&s, v);
  s += "tail";
  std::string_view in(s);
  uint64_t out = 0;
  RC_ASSERT(GetVarint64(&in, &out));
  RC_ASSERT(out == v);
  RC_ASSERT(in == "tail");
}

}  // namespace
}  // namespace tideskv::coding
