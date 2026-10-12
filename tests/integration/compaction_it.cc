#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "support/test_server.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace tideskv::test {
namespace {

int64_t Info(RespClient& c, const std::string& field) {
  const std::string info = c.Command({"INFO", "persistence"}).str;
  const size_t at = info.find(field + ":");
  return at == std::string::npos ? -1 : std::stoll(info.substr(at + field.size() + 1));
}

int64_t WaitAtLeast(RespClient& c, const std::string& field, int64_t want) {
  int64_t v = Info(c, field);
  for (int i = 0; i < 200 && v < want; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    v = Info(c, field);
  }
  return v;
}

TEST(CompactionIT, CollectionDeletesAndFlushDbCompactInTheBackground) {
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--compaction-collection-deletes", "3"}});
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(Info(*c, "background_compactions"), 0);
  for (const char* k : {"h1", "h2", "h3"}) {
    EXPECT_EQ(c->Command({"HSET", k, "f", "v"}).integer, 1);
    EXPECT_EQ(c->Command({"DEL", k}).integer, 1);
  }
  EXPECT_GE(WaitAtLeast(*c, "background_compactions", 2), 2);  // whole subkey + zscore CFs
  const int64_t before = Info(*c, "background_compactions");
  EXPECT_EQ(c->Command({"SET", "k", "v"}).str, "OK");
  EXPECT_EQ(c->Command({"FLUSHDB"}).str, "OK");
  EXPECT_GE(WaitAtLeast(*c, "background_compactions", before + 4), before + 4);  // 4 keyspace CFs
  EXPECT_EQ(Info(*c, "background_compaction_failures"), 0);
  EXPECT_EQ(c->Command({"CONFIG", "SET", "compaction-collection-deletes", "0"}).str, "OK");
}

}  // namespace
}  // namespace tideskv::test
