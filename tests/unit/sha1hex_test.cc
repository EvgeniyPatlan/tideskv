#include "script/sha1hex.h"

#include <gtest/gtest.h>

namespace tideskv {
namespace {

TEST(Sha1Hex, RedisTestVectors) {  // tst:601-604, tst:113-120, tst:571-580
  EXPECT_EQ(Sha1Hex(""), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  EXPECT_EQ(Sha1Hex("Pizza & Mandolino"), "74822d82031af7493c20eefa13bd07ec4fada82f");
  EXPECT_EQ(Sha1Hex("return redis.call('get',KEYS[1])"), "fd758d1589d044dd850a6f05d52f2eefd27f033f");
  EXPECT_EQ(Sha1Hex("return 1+1"), "a27e7e8a43702b7046d4f6a7ccf5b60cef6b9bd9");
  EXPECT_EQ(Sha1Hex(std::string(1000000, 'a')), "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
  EXPECT_EQ(Sha1Hex(std::string("a\0b", 3)).size(), 40u);
}

}  // namespace
}  // namespace tideskv
