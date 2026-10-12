#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "support/temp_dir.h"
#include "support/test_server.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace tideskv::test {
namespace {

int64_t InfoField(RespClient& c, const std::string& section, const std::string& field) {
  const std::string info = c.Command({"INFO", section}).str;
  const size_t at = info.find(field + ":");
  if (at == std::string::npos) return -1;
  return std::stoll(info.substr(at + field.size() + 1));
}

TEST(PersistenceIT, ValuesAndTtlsSurviveRestart) {  // Review Focus #5
  TempDir dir("tideskv-persist");
  {
    TestServer s(TIDESKV_SERVER_BIN, {.dir = dir.str()});
    auto c = s.Connect();
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->Command({"SET", "plain", "v1"}).str, "OK");
    EXPECT_EQ(c->Command({"SET", "ttl", "v2", "PX", "100000"}).str, "OK");
    EXPECT_EQ(c->Command({"SELECT", "5"}).str, "OK");
    EXPECT_EQ(c->Command({"SET", "db5", "v3"}).str, "OK");
    EXPECT_EQ(s.Terminate(), 0);
  }
  TestServer s(TIDESKV_SERVER_BIN, {.dir = dir.str()});
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"GET", "plain"}).str, "v1");
  const int64_t pttl = c->Command({"PTTL", "ttl"}).integer;
  EXPECT_GT(pttl, 90000);
  EXPECT_LE(pttl, 100000);
  EXPECT_EQ(c->Command({"SELECT", "5"}).str, "OK");
  EXPECT_EQ(c->Command({"GET", "db5"}).str, "v3");
}

TEST(PersistenceIT, KeysThatExpiredWhileDownAreHiddenAndReaped) {  // Review Focus #5
  TempDir dir("tideskv-persist");
  {
    TestServer s(TIDESKV_SERVER_BIN, {.dir = dir.str()});
    auto c = s.Connect();
    ASSERT_NE(c, nullptr);
    for (int i = 0; i < 50; ++i) {
      ASSERT_EQ(c->Command({"SET", "e" + std::to_string(i), "v", "PX", "3000"}).str, "OK");
    }
    // Still live and not yet reaped, so the later count really comes from the restart.
    EXPECT_EQ(c->Command({"DBSIZE"}).integer, 50);
    EXPECT_EQ(InfoField(*c, "stats", "expired_keys"), 0);
    EXPECT_EQ(s.Terminate(), 0);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(3500));
  TestServer s(TIDESKV_SERVER_BIN, {.dir = dir.str()});
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"GET", "e0"}).type, RespValue::Type::kNull);
  EXPECT_EQ(c->Command({"DBSIZE"}).integer, 0);
  int64_t expired = 0;
  for (int i = 0; i < 100 && expired < 50; ++i) {
    expired = InfoField(*c, "stats", "expired_keys");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_EQ(expired, 50);
}

TEST(PersistenceIT, InfoReportsStorageFields) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  const std::string info = c->Command({"INFO", "persistence"}).str;
  EXPECT_NE(info.find("compression:"), std::string::npos);
  EXPECT_NE(info.find("tombstones:"), std::string::npos);
}

TEST(PersistenceIT, FlushDbOnHighDbLeavesOtherDbsOnRealBackend) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"SET", "zero", "v0"}).str, "OK");
  EXPECT_EQ(c->Command({"SELECT", "15"}).str, "OK");
  EXPECT_EQ(c->Command({"SET", "fifteen", "v15"}).str, "OK");
  EXPECT_EQ(c->Command({"DBSIZE"}).integer, 1);
  EXPECT_EQ(c->Command({"FLUSHDB"}).str, "OK");
  EXPECT_EQ(c->Command({"DBSIZE"}).integer, 0);
  EXPECT_EQ(c->Command({"SELECT", "0"}).str, "OK");
  EXPECT_EQ(c->Command({"GET", "zero"}).str, "v0");
  EXPECT_EQ(c->Command({"DBSIZE"}).integer, 1);
}

}  // namespace
}  // namespace tideskv::test
