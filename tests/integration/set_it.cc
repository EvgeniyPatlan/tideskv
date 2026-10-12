#include <gtest/gtest.h>

#include "support/test_server.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace tideskv::test {
namespace {

using Type = RespValue::Type;

TEST(SetIT, AlgebraAndResp3Sets) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  std::vector<std::string> a = {"SADD", "a"}, b = {"SADD", "b"};
  for (int i = 0; i < 1000; ++i) a.push_back(std::to_string(i));
  for (int i = 500; i < 1500; ++i) b.push_back(std::to_string(i));
  EXPECT_EQ(c->Command(a).integer, 1000);
  EXPECT_EQ(c->Command(b).integer, 1000);
  EXPECT_EQ(c->Command({"SINTERCARD", "2", "a", "b"}).integer, 500);
  EXPECT_EQ(c->Command({"SUNIONSTORE", "u", "a", "b"}).integer, 1500);
  EXPECT_EQ(c->Command({"SDIFFSTORE", "d", "a", "b"}).integer, 500);
  ASSERT_EQ(c->Command({"HELLO", "3"}).type, Type::kMap);
  EXPECT_EQ(c->Command({"SMEMBERS", "d"}).type, Type::kSet);
  const RespValue popped = c->Command({"SPOP", "d", "10"});
  EXPECT_EQ(popped.type, Type::kSet);
  EXPECT_EQ(popped.elements.size(), 10u);
  EXPECT_EQ(c->Command({"SCARD", "d"}).integer, 490);
  EXPECT_EQ(c->Command({"SRANDMEMBER", "d", "-3"}).type, Type::kArray);
}

}  // namespace
}  // namespace tideskv::test
