#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <string>
#include <thread>

#include "support/test_server.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace tideskv::test {
namespace {

using Type = RespValue::Type;
using namespace std::chrono_literals;

int64_t InfoField(RespClient& c, const std::string& section, const std::string& field) {
  const std::string info = c.Command({"INFO", section}).str;
  const size_t at = info.find(field + ":");
  return at == std::string::npos ? -1 : std::stoll(info.substr(at + field.size() + 1));
}

bool WaitFor(const std::function<bool()>& cond, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (cond()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return cond();
}

TEST(ListZsetIT, Resp3ShapesFromARealClient) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"HELLO", "3"}).type, RespValue::Type::kMap);
  EXPECT_EQ(c->Command({"ZADD", "z", "1.5", "a", "2", "b"}).integer, 2);
  const RespValue r = c->Command({"ZRANGE", "z", "0", "-1", "WITHSCORES"});
  ASSERT_EQ(r.elements.size(), 2u);
  EXPECT_EQ(r.elements[0].elements[1].type, RespValue::Type::kDouble);
  EXPECT_EQ(r.elements[0].elements[1].str, "1.5");
  EXPECT_EQ(c->Command({"ZSCORE", "z", "b"}).str, "2");
}

TEST(ListZsetIT, LargeListAcrossManyFlushes) {
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--memtable-size", "1mb"}});
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  for (int i = 0; i < 20000; ++i)
    ASSERT_EQ(c->Command({"RPUSH", "l", std::string(64, 'x') + std::to_string(i)}).integer, i + 1);
  EXPECT_EQ(c->Command({"LINDEX", "l", "12345"}).str, std::string(64, 'x') + "12345");
  EXPECT_EQ(c->Command({"LINSERT", "l", "BEFORE", std::string(64, 'x') + "10000", "mid"}).integer, 20001);
  EXPECT_EQ(c->Command({"LINDEX", "l", "10000"}).str, "mid");
  EXPECT_EQ(c->Command({"LTRIM", "l", "100", "199"}).str, "OK");
  EXPECT_EQ(c->Command({"LLEN", "l"}).integer, 100);
}

TEST(ListZsetIT, ExpiredListsAndZsetsAreReaped) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  ASSERT_EQ(c->Command({"RPUSH", "l", "a"}).integer, 1);
  ASSERT_EQ(c->Command({"ZADD", "z", "1", "a"}).integer, 1);
  ASSERT_EQ(c->Command({"PEXPIRE", "l", "50"}).integer, 1);
  ASSERT_EQ(c->Command({"PEXPIRE", "z", "50"}).integer, 1);
  ASSERT_TRUE(WaitFor([&] { return InfoField(*c, "stats", "expired_keys") == 2; }));
  EXPECT_EQ(c->Command({"DBSIZE"}).integer, 0);
}

TEST(ListZsetIT, RenameAcrossSlotsKeepsElements) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  ASSERT_EQ(c->Command({"RPUSH", "a", "1", "2", "3"}).integer, 3);
  ASSERT_EQ(c->Command({"ZADD", "za", "1", "x", "2", "y"}).integer, 2);
  EXPECT_EQ(c->Command({"RENAME", "a", "b"}).str, "OK");
  EXPECT_EQ(c->Command({"RENAME", "za", "zb"}).str, "OK");
  EXPECT_EQ(c->Command({"LRANGE", "b", "0", "-1"}).elements.size(), 3u);
  EXPECT_EQ(c->Command({"ZRANGEBYSCORE", "zb", "2", "2"}).elements[0].str, "y");
}

}  // namespace
}  // namespace tideskv::test
