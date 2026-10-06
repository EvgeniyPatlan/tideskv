#include "common/crc16.h"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

namespace tideskv {
namespace {

TEST(Crc16, StandardCheckValue) { EXPECT_EQ(Crc16("123456789"), 0x31C3); }

TEST(KeySlot, MatchesRedis) {
  EXPECT_EQ(KeySlot("foo"), 12182);
  EXPECT_EQ(KeySlot("somekey"), 11058);
  EXPECT_EQ(KeySlot("hello"), 866);
  EXPECT_EQ(KeySlot(""), 0);
}

TEST(KeySlot, HashTagsGroupKeys) {
  EXPECT_EQ(KeySlot("{user1000}.following"), 3443);
  EXPECT_EQ(KeySlot("{user1000}.followers"), 3443);
  EXPECT_EQ(KeySlot("foo{bar}{zap}"), KeySlot("bar"));
}

TEST(KeySlot, EmptyOrUnclosedTagHashesWholeKey) {
  EXPECT_EQ(KeySlot("foo{}{bar}"), 8363);                // empty first tag → whole key
  EXPECT_EQ(KeySlot("foo{{bar}}zap"), KeySlot("{bar"));  // tag is "{bar"
  EXPECT_EQ(KeySlot("foo{bar"), Crc16("foo{bar") % kSlotCount);
}

TEST(KeySlot, BinarySafe) {
  std::string k("a\0b", 3);
  EXPECT_EQ(KeySlot(k), Crc16(k) % kSlotCount);
}

RC_GTEST_PROP(KeySlotProp, AlwaysInRange, (std::string key)) { RC_ASSERT(KeySlot(key) < kSlotCount); }

}  // namespace
}  // namespace tideskv
