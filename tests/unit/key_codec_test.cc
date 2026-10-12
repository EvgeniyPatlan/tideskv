#include "encoding/key_codec.h"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include "common/crc16.h"

namespace tideskv::encoding {
namespace {

TEST(MetaKey, LayoutIsDbSlotKey) {
  EXPECT_EQ(EncodeMetaKey(3, 0x0102, "ab"), std::string("\x03\x01\x02"
                                                        "ab",
                                                        5));
}

TEST(MetaKey, MetaKeyForComputesSlot) {
  EXPECT_EQ(MetaKeyFor(2, "{user}:1"), EncodeMetaKey(2, KeySlot("user"), "{user}:1"));
}

TEST(MetaKey, EmptyUserKeyIsAllowed) {  // Redis permits SET "" x
  MetaKey mk;
  ASSERT_TRUE(DecodeMetaKey(EncodeMetaKey(0, 5, ""), &mk));
  EXPECT_EQ(mk.db, 0);
  EXPECT_EQ(mk.slot, 5);
  EXPECT_EQ(mk.user_key, "");
}

TEST(MetaKey, DecodeRejectsShortOrBadSlotOrBadDb) {
  MetaKey mk;
  EXPECT_FALSE(DecodeMetaKey(std::string("\x00\x01", 2), &mk));       // no room for slot
  EXPECT_FALSE(DecodeMetaKey(std::string("\x00\x40\x00k", 4), &mk));  // slot 16384
  EXPECT_FALSE(DecodeMetaKey(std::string("\x10\x00\x00k", 4), &mk));  // db 16
  EXPECT_TRUE(DecodeMetaKey(std::string("\x0f\x3f\xffk", 4), &mk));   // db 15, slot 16383
}

TEST(MetaKey, PrefixesMatch) {
  const std::string enc = EncodeMetaKey(2, 77, "user:1");
  EXPECT_EQ(enc.rfind(DbPrefix(2), 0), 0u);
  EXPECT_EQ(enc.rfind(MetaSlotPrefix(2, 77), 0), 0u);
  EXPECT_NE(enc.rfind(DbPrefix(3), 0), 0u);
  EXPECT_NE(enc.rfind(MetaSlotPrefix(2, 78), 0), 0u);
}

TEST(MetaKey, AllKeysOfOneDbAreContiguous) {
  // Every key of db 1 sorts after every key of db 0 and before every key of db 2.
  EXPECT_LT(EncodeMetaKey(0, 16383, std::string(300, '\xff')), EncodeMetaKey(1, 0, ""));
  EXPECT_LT(EncodeMetaKey(1, 16383, std::string(300, '\xff')), DbPrefix(2));
}

RC_GTEST_PROP(MetaKeyProp, RoundTrips, (std::string key, uint8_t raw_db)) {
  const uint8_t db = raw_db % kNumDatabases;
  const std::string enc = MetaKeyFor(db, key);
  MetaKey mk;
  RC_ASSERT(DecodeMetaKey(enc, &mk));
  RC_ASSERT(mk.db == db);
  RC_ASSERT(mk.slot == KeySlot(key));
  RC_ASSERT(mk.user_key == key);
}

RC_GTEST_PROP(MetaKeyProp, OrderWithinSlotFollowsUserKey, (std::string a, std::string b)) {
  RC_ASSERT((a < b) == (EncodeMetaKey(0, 9, a) < EncodeMetaKey(0, 9, b)));
}

TEST(ExpireKey, LayoutIsDbTimeSlotKey) {
  EXPECT_EQ(EncodeExpireKey(1, 0x0102030405060708ULL, 0x0a0b, "k"),
            std::string("\x01\x01\x02\x03\x04\x05\x06\x07\x08\x0a\x0bk", 12));
}

TEST(ExpireKey, SortsByDbThenTime) {
  EXPECT_LT(EncodeExpireKey(0, 1000, 16383, "zzz"), EncodeExpireKey(0, 1001, 0, "a"));
  EXPECT_LT(EncodeExpireKey(0, UINT64_MAX, 0, "a"), EncodeExpireKey(1, 0, 0, "a"));
}

TEST(ExpireKey, UpperBoundSeparatesDueFromFuture) {
  const std::string bound = ExpireUpperBound(4, 5000);
  EXPECT_LT(ExpireKeyFor(4, 5000, std::string(200, '\xff')), bound);  // due (== now)
  EXPECT_GE(ExpireKeyFor(4, 5001, ""), bound);                        // future
  EXPECT_GE(ExpireKeyFor(5, 0, ""), bound);                           // next db
  EXPECT_EQ(ExpireUpperBound(4, UINT64_MAX), DbPrefix(5));            // no overflow
}

TEST(ExpireKey, DecodeRejectsShortOrBadSlot) {
  ExpireKey ek;
  EXPECT_FALSE(DecodeExpireKey(std::string("\x00\x00\x00", 3), &ek));
  EXPECT_FALSE(DecodeExpireKey(EncodeExpireKey(0, 1, 0, "k").substr(0, 10), &ek));  // slot cut off
  EXPECT_FALSE(DecodeExpireKey(std::string(9, '\0') + std::string("\x40\x00k", 3), &ek));
}

RC_GTEST_PROP(ExpireKeyProp, RoundTrips, (uint64_t ts, std::string key, uint8_t raw_db)) {
  const uint8_t db = raw_db % kNumDatabases;
  const std::string enc = ExpireKeyFor(db, ts, key);  // user_key views into enc
  ExpireKey ek;
  RC_ASSERT(DecodeExpireKey(enc, &ek));
  RC_ASSERT(ek.db == db);
  RC_ASSERT(ek.expire_ms == ts);
  RC_ASSERT(ek.slot == KeySlot(key));
  RC_ASSERT(ek.user_key == key);
}

}  // namespace
}  // namespace tideskv::encoding
