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
  return at == std::string::npos ? -1 : std::stoll(info.substr(at + field.size() + 1));
}

TEST(CollectionPersistenceIT, HashesSetsAndTtlsSurviveRestart) {
  TempDir dir("tideskv-coll");
  {
    TestServer s(TIDESKV_SERVER_BIN, {.dir = dir.str()});
    auto c = s.Connect();
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->Command({"HSET", "h", "f", "v", "g", "w"}).integer, 2);
    EXPECT_EQ(c->Command({"SADD", "s", "a", "b"}).integer, 2);
    EXPECT_EQ(c->Command({"PEXPIRE", "s", "100000"}).integer, 1);
    EXPECT_EQ(s.Terminate(), 0);
  }
  TestServer s(TIDESKV_SERVER_BIN, {.dir = dir.str()});
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"HGET", "h", "g"}).str, "w");
  EXPECT_EQ(c->Command({"HLEN", "h"}).integer, 2);
  EXPECT_EQ(c->Command({"SCARD", "s"}).integer, 2);
  const int64_t pttl = c->Command({"PTTL", "s"}).integer;
  EXPECT_GT(pttl, 90000);
  EXPECT_LE(pttl, 100000);
}

// All keys share one hash slot ({o}), so subkeys live under (db, slot, oid) alone: a new hash given
// an old hash's oid would list that old hash's surviving fields (and the old hash the new field).
TEST(CollectionPersistenceIT, OidsAreNeverReusedAfterKill9) {
  constexpr int kHashes = 100;
  TempDir dir("tideskv-coll");
  {
    TestServer s(TIDESKV_SERVER_BIN, {.dir = dir.str()});  // default (interval) sync mode
    auto c = s.Connect();
    ASSERT_NE(c, nullptr);
    for (int i = 0; i < kHashes; ++i) {
      const std::string n = std::to_string(i);
      ASSERT_EQ(c->Command({"HSET", "{o}old" + n, "of" + n, "v"}).integer, 1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // past one WAL sync interval
    s.Kill9();
  }
  TestServer s(TIDESKV_SERVER_BIN, {.dir = dir.str()});
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  ASSERT_EQ(c->Command({"HLEN", "{o}old0"}).integer, 1);  // the old data survived the kill
  for (int i = 0; i < kHashes; ++i) {
    ASSERT_EQ(c->Command({"HSET", "{o}new" + std::to_string(i), "nf", "v"}).integer, 1);
  }
  for (int i = 0; i < kHashes; ++i) {
    const std::string n = std::to_string(i);
    const RespValue fresh = c->Command({"HGETALL", "{o}new" + n});
    ASSERT_EQ(fresh.elements.size(), 2u) << i << ": " << ToDebugString(fresh);
    EXPECT_EQ(fresh.elements[0].str, "nf");
    const RespValue old = c->Command({"HGETALL", "{o}old" + n});
    ASSERT_EQ(old.elements.size(), 2u) << i << ": " << ToDebugString(old);
    EXPECT_EQ(old.elements[0].str, "of" + n);
  }
}

TEST(CollectionPersistenceIT, CollectionsThatExpiredWhileDownAreReaped) {
  TempDir dir("tideskv-coll");
  {
    TestServer s(TIDESKV_SERVER_BIN, {.dir = dir.str()});
    auto c = s.Connect();
    ASSERT_NE(c, nullptr);
    for (int i = 0; i < 20; ++i) {
      const std::string k = "h" + std::to_string(i);
      ASSERT_EQ(c->Command({"HSET", k, "f", "v"}).integer, 1);
      ASSERT_EQ(c->Command({"PEXPIRE", k, "3000"}).integer, 1);
    }
    EXPECT_EQ(s.Terminate(), 0);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(3500));
  TestServer s(TIDESKV_SERVER_BIN, {.dir = dir.str()});
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"DBSIZE"}).integer, 0);
  int64_t expired = 0;
  for (int i = 0; i < 100 && expired < 20; ++i) {
    expired = InfoField(*c, "stats", "expired_keys");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_EQ(expired, 20);
}

}  // namespace
}  // namespace tideskv::test
