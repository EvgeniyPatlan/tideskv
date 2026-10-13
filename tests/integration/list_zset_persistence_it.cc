#include <gtest/gtest.h>

#include <string>

#include "support/test_server.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace tideskv::test {
namespace {

TEST(ListZsetPersistenceIT, ListsAndZsetsSurviveRestart) {
  TempDir dir("tideskv-lz");
  {
    TestServer s(TIDESKV_SERVER_BIN, {.dir = dir.str()});
    auto c = s.Connect();
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->Command({"RPUSH", "l", "a", "b", "c"}).integer, 3);
    EXPECT_EQ(c->Command({"LPOP", "l"}).str, "a");
    EXPECT_EQ(c->Command({"ZADD", "z", "1", "x", "2", "y"}).integer, 2);
    EXPECT_EQ(c->Command({"PEXPIRE", "z", "100000"}).integer, 1);
    EXPECT_EQ(s.Terminate(), 0);
  }
  TestServer s(TIDESKV_SERVER_BIN, {.dir = dir.str()});
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  const RespValue l = c->Command({"LRANGE", "l", "0", "-1"});
  ASSERT_EQ(l.elements.size(), 2u);
  EXPECT_EQ(l.elements[0].str, "b");
  EXPECT_EQ(c->Command({"ZRANK", "z", "y"}).integer, 1);
  EXPECT_GT(c->Command({"PTTL", "z"}).integer, 0);
  EXPECT_EQ(c->Command({"RPUSH", "l", "d"}).integer, 3);  // head/tail survived: appends after "c"
  EXPECT_EQ(c->Command({"LINDEX", "l", "-1"}).str, "d");
}

TEST(ListZsetPersistenceIT, AcknowledgedPushesSurviveKill9WithSyncFull) {
  TempDir dir("tideskv-lz9");
  {
    TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--sync-mode", "full"}, .dir = dir.str()});
    auto c = s.Connect();
    ASSERT_NE(c, nullptr);
    for (int i = 0; i < 200; ++i) ASSERT_EQ(c->Command({"RPUSH", "l", std::to_string(i)}).integer, i + 1);
    for (int i = 0; i < 200; ++i)
      ASSERT_EQ(c->Command({"ZADD", "z", std::to_string(i), std::to_string(i)}).integer, 1);
    s.Kill9();
  }
  TestServer s(TIDESKV_SERVER_BIN, {.dir = dir.str()});
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"LLEN", "l"}).integer, 200);
  EXPECT_EQ(c->Command({"ZCARD", "z"}).integer, 200);
  EXPECT_EQ(c->Command({"ZSCORE", "z", "199"}).str, "199");
}

}  // namespace
}  // namespace tideskv::test
