#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "support/test_server.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace tideskv::test {
namespace {

using Type = RespValue::Type;

TEST(StringsIT, SetGetMsetMgetOverTheWire) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  const std::string big(1 << 20, 'x');  // 1 MiB value
  EXPECT_EQ(c->Command({"SET", "big", big}).str, "OK");
  EXPECT_EQ(c->Command({"GET", "big"}).str, big);
  EXPECT_EQ(c->Command({"MSET", "a", "1", "b", "2"}).str, "OK");
  const RespValue r = c->Command({"MGET", "a", "b", "nope"});
  ASSERT_EQ(r.type, Type::kArray);
  ASSERT_EQ(r.elements.size(), 3u);
  EXPECT_EQ(r.elements[0].str, "1");
  EXPECT_EQ(r.elements[1].str, "2");
  EXPECT_EQ(r.elements[2].type, Type::kNull);
}

TEST(StringsIT, PxExpiresInRealTime) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"SET", "k", "v", "PX", "100"}).str, "OK");
  EXPECT_EQ(c->Command({"GET", "k"}).str, "v");
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  EXPECT_EQ(c->Command({"GET", "k"}).type, Type::kNull);
}

TEST(StringsIT, Resp3NullForMissing) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  ASSERT_EQ(c->Command({"HELLO", "3"}).type, Type::kMap);
  EXPECT_EQ(c->Command({"GET", "nope"}).type, Type::kNull);
}

}  // namespace
}  // namespace tideskv::test
