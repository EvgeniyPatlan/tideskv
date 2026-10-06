#include "encoding/key_codec.h"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include "common/crc16.h"

namespace tideskv::encoding {
namespace {

TEST(MetaKey, Layout) {
  EXPECT_EQ(EncodeMetaKey(0x0102, 3, "ab"), std::string("\x01\x02\x03" "ab", 5));
}

TEST(MetaKey, EmptyUserKeyIsAllowed) {  // Redis permits SET "" x
  const std::string enc = EncodeMetaKey(5, 0, "");
  MetaKey mk;
  ASSERT_TRUE(DecodeMetaKey(enc, &mk));
  EXPECT_EQ(mk.user_key, "");
}

TEST(MetaKey, DecodeRejectsShortOrBadSlot) {
  MetaKey mk;
  EXPECT_FALSE(DecodeMetaKey(std::string("\x00\x01", 2), &mk));
  EXPECT_FALSE(DecodeMetaKey(std::string("\x40\x00\x00k", 4), &mk));  // slot 16384
}

TEST(MetaKey, PrefixesMatch) {
  const std::string enc = EncodeMetaKey(77, 2, "user:1");
  EXPECT_EQ(enc.rfind(MetaSlotPrefix(77), 0), 0u);
  EXPECT_EQ(enc.rfind(MetaDbPrefix(77, 2), 0), 0u);
  EXPECT_NE(enc.rfind(MetaDbPrefix(77, 3), 0), 0u);
}

RC_GTEST_PROP(MetaKeyProp, RoundTrips, (std::string key, uint8_t db)) {
  const uint16_t slot = KeySlot(key);
  const std::string enc = EncodeMetaKey(slot, db, key);
  MetaKey mk;
  RC_ASSERT(DecodeMetaKey(enc, &mk));
  RC_ASSERT(mk.slot == slot);
  RC_ASSERT(mk.db == db);
  RC_ASSERT(mk.user_key == key);
}

RC_GTEST_PROP(MetaKeyProp, OrderWithinDbFollowsUserKey, (std::string a, std::string b)) {
  RC_ASSERT((a < b) == (EncodeMetaKey(9, 0, a) < EncodeMetaKey(9, 0, b)));
}

TEST(ExpireKey, SortsByTimeFirst) {
  EXPECT_LT(EncodeExpireKey(1000, 16383, 15, "zzz"), EncodeExpireKey(1001, 0, 0, "a"));
}

RC_GTEST_PROP(ExpireKeyProp, RoundTrips, (uint64_t ts, std::string key, uint8_t db)) {
  const uint16_t slot = KeySlot(key);
  const std::string enc = EncodeExpireKey(ts, slot, db, key);
  ExpireKey ek;
  RC_ASSERT(DecodeExpireKey(enc, &ek));
  RC_ASSERT(ek.expire_ms == ts);
  RC_ASSERT(ek.slot == slot);
  RC_ASSERT(ek.db == db);
  RC_ASSERT(ek.user_key == key);
}

}  // namespace
}  // namespace tideskv::encoding
