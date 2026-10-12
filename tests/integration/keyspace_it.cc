#include <gtest/gtest.h>

#include "support/test_server.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace tideskv::test {
namespace {

TEST(KeyspaceIT, SelectIsPerConnection) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto c1 = s.Connect();
  auto c2 = s.Connect();
  ASSERT_NE(c1, nullptr);
  ASSERT_NE(c2, nullptr);
  EXPECT_EQ(c1->Command({"SELECT", "3"}).str, "OK");
  EXPECT_EQ(c1->Command({"SET", "k", "in3"}).str, "OK");
  EXPECT_EQ(c2->Command({"GET", "k"}).type, RespValue::Type::kNull);
  EXPECT_EQ(c2->Command({"SELECT", "3"}).str, "OK");
  EXPECT_EQ(c2->Command({"GET", "k"}).str, "in3");
}

TEST(KeyspaceIT, FlushAllAndDbSize) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  for (int i = 0; i < 100; ++i) ASSERT_EQ(c->Command({"SET", "k" + std::to_string(i), "v"}).str, "OK");
  EXPECT_EQ(c->Command({"DBSIZE"}).integer, 100);
  EXPECT_EQ(c->Command({"DEL", "k0", "k1", "nope"}).integer, 2);
  EXPECT_EQ(c->Command({"DBSIZE"}).integer, 98);
  EXPECT_EQ(c->Command({"FLUSHALL"}).str, "OK");
  EXPECT_EQ(c->Command({"DBSIZE"}).integer, 0);
}

}  // namespace
}  // namespace tideskv::test
