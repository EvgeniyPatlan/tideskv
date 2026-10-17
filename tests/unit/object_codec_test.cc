#include "encoding/object_codec.h"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include <cmath>

namespace tideskv::encoding {
namespace {

TEST(ObjectCodec, PrefixIsElevenBytesDbSlotOid) {
  EXPECT_EQ(SubkeyPrefix(1, 42, 9).size(), kObjectPrefixSize);
  EXPECT_EQ(ZScorePrefix(1, 42, 9).size(), kObjectPrefixSize);
  EXPECT_EQ(SubkeyPrefix(2, 0x0102, 0x0A), std::string("\x02\x01\x02\0\0\0\0\0\0\0\x0A", 11));
}

TEST(ObjectCodec, SubkeyStartsWithItsPrefixAndDbPrefix) {
  const std::string enc = EncodeSubkey(4, 3, 42, "name");
  EXPECT_EQ(enc.rfind(SubkeyPrefix(4, 3, 42), 0), 0u);
  EXPECT_EQ(enc[0], '\x04');  // FLUSHDB's DeletePrefix([db]) covers it
}

RC_GTEST_PROP(ObjectCodecProp, SubkeyRoundTrips, (uint64_t oid, std::string field)) {
  const std::string enc = EncodeSubkey(0, 100, oid, field);
  Subkey sk;
  RC_ASSERT(DecodeSubkey(enc, &sk));
  RC_ASSERT(sk.db == 0 && sk.slot == 100);
  RC_ASSERT(sk.oid == oid);
  RC_ASSERT(sk.field == field);
}

// Deleting object A by prefix must never touch object B.
RC_GTEST_PROP(ObjectCodecProp, DistinctOidsNeverSharePrefix, (uint64_t a, uint64_t b, std::string field)) {
  RC_PRE(a != b);
  RC_ASSERT(EncodeSubkey(0, 7, a, field).rfind(SubkeyPrefix(0, 7, b), 0) != 0u);
}

RC_GTEST_PROP(ObjectCodecProp, ZScoreOrdersByScoreThenMember,
              (double s1, double s2, std::string m1, std::string m2)) {
  RC_PRE(!std::isnan(s1) && !std::isnan(s2));
  const std::string k1 = EncodeZScoreKey(0, 1, 9, s1, m1);
  const std::string k2 = EncodeZScoreKey(0, 1, 9, s2, m2);
  const double n1 = s1 == 0.0 ? 0.0 : s1;  // -0.0 and +0.0 are the same score
  const double n2 = s2 == 0.0 ? 0.0 : s2;
  if (n1 != n2) {
    RC_ASSERT((n1 < n2) == (k1 < k2));
  } else {
    RC_ASSERT((m1 < m2) == (k1 < k2));
  }
}

TEST(ObjectCodec, ZScoreRoundTrips) {
  const std::string enc = EncodeZScoreKey(3, 5, 77, -2.5, std::string("m\0x", 3));
  ZScoreKey zk;
  ASSERT_TRUE(DecodeZScoreKey(enc, &zk));
  EXPECT_EQ(zk.db, 3);
  EXPECT_EQ(zk.slot, 5);
  EXPECT_EQ(zk.oid, 77u);
  EXPECT_EQ(zk.score, -2.5);
  EXPECT_EQ(zk.member, std::string_view("m\0x", 3));
}

TEST(ObjectCodec, DecodeRejectsShortInput) {
  Subkey sk;
  ZScoreKey zk;
  EXPECT_FALSE(DecodeSubkey("short", &sk));
  EXPECT_FALSE(DecodeZScoreKey(SubkeyPrefix(0, 1, 1) + "1234567", &zk));  // only 7 score bytes
}

TEST(ObjectCodec, ListIndexOrderingAndRoundTrip) {
  EXPECT_LT(EncodeListIndex(kListInitialIndex - 1), EncodeListIndex(kListInitialIndex));
  uint64_t idx = 0;
  ASSERT_TRUE(DecodeListIndex(EncodeListIndex(kListInitialIndex + 5), &idx));
  EXPECT_EQ(idx, kListInitialIndex + 5);
  EXPECT_FALSE(DecodeListIndex("123", &idx));
}

TEST(ObjectCodec, PeeksAndRewritesTheOidOfSubkeyAndZscoreKeys) {  // M7b D2
  std::string sub = EncodeSubkey(0, 3300, 42, "field");
  uint64_t oid = 0;
  ASSERT_TRUE(PeekObjectOid(sub, &oid));
  EXPECT_EQ(oid, 42u);
  ASSERT_TRUE(RewriteObjectOid(&sub, 1000));
  EXPECT_EQ(sub, EncodeSubkey(0, 3300, 1000, "field"));
  std::string z = EncodeZScoreKey(0, 3300, 42, -2.5, "m");
  ASSERT_TRUE(RewriteObjectOid(&z, 7));
  EXPECT_EQ(z, EncodeZScoreKey(0, 3300, 7, -2.5, "m"));
  std::string prefix = SubkeyPrefix(0, 3300, 42);
  ASSERT_TRUE(RewriteObjectOid(&prefix, 9));
  EXPECT_EQ(prefix, SubkeyPrefix(0, 3300, 9));
  std::string short_key("\x00\x0c", 2);
  EXPECT_FALSE(PeekObjectOid(short_key, &oid));
  EXPECT_FALSE(RewriteObjectOid(&short_key, 1));
}

}  // namespace
}  // namespace tideskv::encoding
