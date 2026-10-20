#include "encoding/key_layout.h"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include "common/crc16.h"
#include "encoding/key_codec.h"
#include "encoding/object_codec.h"

// The slotless key layout (design 2026-10-20): the same keys as the slotted layout without the slot bytes.
namespace tideskv::encoding {
namespace {

TEST(KeyLayout, ScopedLayoutRestoresThePreviousOne) {
  const KeyLayout before = CurrentKeyLayout();
  {
    const ScopedKeyLayout slotless(KeyLayout::kSlotless);
    EXPECT_TRUE(Slotless());
    {
      const ScopedKeyLayout slotted(KeyLayout::kSlotted);
      EXPECT_FALSE(Slotless());
    }
    EXPECT_TRUE(Slotless());
  }
  EXPECT_EQ(CurrentKeyLayout(), before);
}

TEST(KeyLayout, InitIsRefusedTheSecondTime) {
  const ScopedKeyLayout keep(CurrentKeyLayout());  // whatever Init sets is undone at the end
  (void)InitKeyLayout(KeyLayout::kSlotless);       // the first call in this process, unless main made one
  EXPECT_FALSE(InitKeyLayout(KeyLayout::kSlotted));
  EXPECT_FALSE(InitKeyLayout(KeyLayout::kSlotless));
}

TEST(KeyLayout, NamesRoundTrip) {
  for (KeyLayout l : {KeyLayout::kSlotted, KeyLayout::kSlotless})
    EXPECT_EQ(ParseKeyLayout(KeyLayoutName(l)), l);
  EXPECT_EQ(KeyLayoutName(KeyLayout::kSlotless), "slotless");
  EXPECT_FALSE(ParseKeyLayout("SLOTLESS").has_value());
  EXPECT_FALSE(ParseKeyLayout("").has_value());
}

TEST(KeyLayout, LayoutSlotIsZeroWhenSlotless) {
  {
    const ScopedKeyLayout slotted(KeyLayout::kSlotted);
    EXPECT_EQ(LayoutSlot("{user}:1"), KeySlot("user"));
  }
  const ScopedKeyLayout slotless(KeyLayout::kSlotless);
  EXPECT_EQ(LayoutSlot("{user}:1"), 0);
}

TEST(SlotlessCodec, MetaAndExpireBytes) {
  const ScopedKeyLayout slotless(KeyLayout::kSlotless);
  EXPECT_EQ(EncodeMetaKey(3, 0x0102, "ab"), std::string("\x03"
                                                        "ab",
                                                        3));  // the slot argument is ignored
  EXPECT_EQ(MetaKeyFor(3, "ab"), std::string("\x03"
                                             "ab",
                                             3));
  EXPECT_EQ(EncodeExpireKey(1, 0x0102030405060708ULL, 0x0a0b, "k"),
            std::string("\x01\x01\x02\x03\x04\x05\x06\x07\x08k", 10));
  EXPECT_EQ(ExpireKeyFor(1, 0x0102030405060708ULL, "k"), EncodeExpireKey(1, 0x0102030405060708ULL, 0, "k"));
  EXPECT_EQ(ExpireUpperBound(4, 5000), ExpireUpperBound(4, 5000));  // [db][now+1]: layout-independent
  EXPECT_LT(ExpireKeyFor(4, 5000, std::string(200, '\xff')), ExpireUpperBound(4, 5000));
  EXPECT_GE(ExpireKeyFor(4, 5001, ""), ExpireUpperBound(4, 5000));
}

TEST(SlotlessCodec, MetaAndExpireDecode) {
  const ScopedKeyLayout slotless(KeyLayout::kSlotless);
  const std::string db15_key(
      "\x0f"
      "key",
      4);  // decoded views point into the input: keep it alive
  MetaKey mk;
  ASSERT_TRUE(DecodeMetaKey(db15_key, &mk));
  EXPECT_EQ(mk.db, 15);
  EXPECT_EQ(mk.slot, 0);
  EXPECT_EQ(mk.user_key, "key");
  const std::string empty_key("\x00", 1);
  ASSERT_TRUE(DecodeMetaKey(empty_key, &mk));  // the empty user key
  EXPECT_EQ(mk.user_key, "");
  EXPECT_FALSE(DecodeMetaKey(std::string("\x10k", 2), &mk));  // db 16
  EXPECT_FALSE(DecodeMetaKey("", &mk));
  const std::string expire_key = ExpireKeyFor(2, 77, "user");
  ExpireKey ek;
  ASSERT_TRUE(DecodeExpireKey(expire_key, &ek));
  EXPECT_EQ(ek.db, 2);
  EXPECT_EQ(ek.expire_ms, 77u);
  EXPECT_EQ(ek.slot, 0);
  EXPECT_EQ(ek.user_key, "user");
  EXPECT_FALSE(DecodeExpireKey(std::string("\x00\x01", 2), &ek));  // no room for the time
}

RC_GTEST_PROP(SlotlessCodecProp, MetaRoundTripsAndOrdersByUserKey, (std::string a, std::string b)) {
  const ScopedKeyLayout slotless(KeyLayout::kSlotless);
  const std::string enc = MetaKeyFor(5, a);
  MetaKey mk;
  RC_ASSERT(DecodeMetaKey(enc, &mk));
  RC_ASSERT(mk.db == 5 && mk.user_key == a);
  RC_ASSERT((a < b) == (MetaKeyFor(0, a) < MetaKeyFor(0, b)));  // neighbouring user keys are neighbours
  RC_ASSERT(MetaKeyFor(0, a) < MetaKeyFor(1, b));               // one db is still one contiguous range
}

TEST(SlotlessCodec, ObjectKeysDropTheSlot) {
  const ScopedKeyLayout slotless(KeyLayout::kSlotless);
  EXPECT_EQ(ObjectPrefixSize(), 9u);
  EXPECT_EQ(SubkeyPrefix(2, 0x0102, 0x0A), std::string("\x02\0\0\0\0\0\0\0\x0A", 9));
  EXPECT_EQ(ZScorePrefix(2, 0x0102, 0x0A), SubkeyPrefix(2, 0, 0x0A));
  EXPECT_EQ(EncodeSubkey(4, 3, 42, "name"), SubkeyPrefix(4, 0, 42) + "name");
  const std::string subkey = EncodeSubkey(4, 3, 42, "name");
  Subkey sk;
  ASSERT_TRUE(DecodeSubkey(subkey, &sk));
  EXPECT_EQ(sk.db, 4);
  EXPECT_EQ(sk.slot, 0);
  EXPECT_EQ(sk.oid, 42u);
  EXPECT_EQ(sk.field, "name");
  const std::string zscore_key = EncodeZScoreKey(3, 5, 77, -2.5, "m");
  ZScoreKey zk;
  ASSERT_TRUE(DecodeZScoreKey(zscore_key, &zk));
  EXPECT_EQ(zk.oid, 77u);
  EXPECT_EQ(zk.score, -2.5);
  EXPECT_EQ(zk.member, "m");
  EXPECT_FALSE(DecodeSubkey("short", &sk));
  EXPECT_FALSE(DecodeZScoreKey(SubkeyPrefix(0, 0, 1) + "1234567", &zk));
}

TEST(SlotlessCodec, PeeksAndRewritesTheOidAtOffsetOne) {
  const ScopedKeyLayout slotless(KeyLayout::kSlotless);
  std::string sub = EncodeSubkey(0, 0, 42, "field");
  uint64_t oid = 0;
  ASSERT_TRUE(PeekObjectOid(sub, &oid));
  EXPECT_EQ(oid, 42u);
  ASSERT_TRUE(RewriteObjectOid(&sub, 1000));
  EXPECT_EQ(sub, EncodeSubkey(0, 0, 1000, "field"));
  std::string prefix = SubkeyPrefix(0, 0, 42);  // exactly ObjectPrefixSize() bytes
  ASSERT_TRUE(PeekObjectOid(prefix, &oid));
  EXPECT_EQ(oid, 42u);
  std::string short_key(8, '\0');
  EXPECT_FALSE(PeekObjectOid(short_key, &oid));
  EXPECT_FALSE(RewriteObjectOid(&short_key, 1));
}

TEST(SlotlessCodec, ObjectsCreatedInOrderSitInOrder) {  // oids grow, so one object's entries stay together
  const ScopedKeyLayout slotless(KeyLayout::kSlotless);
  EXPECT_LT(EncodeSubkey(0, 0, 7, std::string(100, '\xff')), EncodeSubkey(0, 0, 8, ""));
  EXPECT_LT(EncodeSubkey(0, 0, UINT64_MAX, "z"), EncodeSubkey(1, 0, 0, ""));
}

TEST(SlottedCodec, ObjectPrefixKeepsElevenBytes) {
  const ScopedKeyLayout slotted(KeyLayout::kSlotted);
  EXPECT_EQ(ObjectPrefixSize(), 11u);
  EXPECT_EQ(MetaKeyFor(2, "{user}:1"), EncodeMetaKey(2, KeySlot("user"), "{user}:1"));
}

}  // namespace
}  // namespace tideskv::encoding
