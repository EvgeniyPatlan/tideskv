#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

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
  constexpr int kFields = 500;
  std::vector<std::string> hset = {"HSET", ""};
  for (int i = 0; i < kFields; ++i) {
    hset.push_back("f" + std::to_string(i));
    hset.push_back("v");
  }
  for (const char* k : {"h1", "h2", "h3"}) {
    hset[1] = k;
    EXPECT_EQ(c->Command(hset).integer, kFields);
    EXPECT_EQ(c->Command({"DEL", k}).integer, 1);
  }
  EXPECT_GE(WaitAtLeast(*c, "background_compactions", 2), 2);  // whole subkey + zscore CFs
  EXPECT_LE(Info(*c, "sstable_keys"), 20);                     // the 1500 deleted fields are physically gone
  const int64_t before = Info(*c, "background_compactions");
  EXPECT_EQ(c->Command({"SET", "k", "v"}).str, "OK");
  EXPECT_EQ(c->Command({"FLUSHDB"}).str, "OK");
  EXPECT_GE(WaitAtLeast(*c, "background_compactions", before + 4), before + 4);  // 4 keyspace CFs
  EXPECT_EQ(Info(*c, "background_compaction_failures"), 0);
  EXPECT_EQ(c->Command({"CONFIG", "SET", "compaction-collection-deletes", "0"}).str, "OK");
}

// Pipelined SETs of key0..key<n-1> in the selected db.
void FillDb(RespClient& c, int n) {
  constexpr int kBatch = 1000;
  for (int i = 0; i < n; i += kBatch) {
    std::string batch;
    for (int j = i; j < i + kBatch && j < n; ++j)
      batch += EncodeCommand({"SET", "key" + std::to_string(j), "x"});
    ASSERT_TRUE(c.SendRaw(batch));
    for (int j = i; j < i + kBatch && j < n; ++j) ASSERT_EQ(c.Read()->str, "OK");
  }
}

// Waits for the background compactions an empty FLUSHDB triggers, whose checkpoint flushes the memtable.
void FlushToSstables(RespClient& c, const std::string& db) {
  const int64_t before = Info(c, "background_compactions");
  EXPECT_EQ(c.Command({"SELECT", "9"}).str, "OK");
  EXPECT_EQ(c.Command({"FLUSHDB"}).str, "OK");
  EXPECT_GE(WaitAtLeast(c, "background_compactions", before + 4), before + 4);
  EXPECT_EQ(c.Command({"SELECT", db}).str, "OK");
}

int64_t WaitAtMost(RespClient& c, const std::string& field, int64_t want) {
  int64_t v = Info(c, field);
  for (int i = 0; i < 200 && v > want; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    v = Info(c, field);
  }
  return v;
}

TEST(CompactionIT, FlushDbPhysicallyReclaimsThatDbsKeys) {
  constexpr int kDead = 60000;  // several 1 MiB memtables' worth
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--memtable-size", "1mb"}});
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"SET", "live0", "v"}).str, "OK");
  EXPECT_EQ(c->Command({"SELECT", "3"}).str, "OK");
  EXPECT_EQ(c->Command({"SET", "live3", "v"}).str, "OK");
  EXPECT_EQ(c->Command({"SELECT", "2"}).str, "OK");
  FillDb(*c, kDead);
  FlushToSstables(*c, "2");
  ASSERT_GE(Info(*c, "sstable_keys"), kDead);
  EXPECT_EQ(c->Command({"FLUSHDB"}).str, "OK");
  // Only the two live keys and a few system records stay on disk once compaction has run.
  EXPECT_LE(WaitAtMost(*c, "sstable_keys", 20), 20);
  EXPECT_EQ(c->Command({"DBSIZE"}).integer, 0);
  EXPECT_EQ(c->Command({"SELECT", "3"}).str, "OK");
  EXPECT_EQ(c->Command({"GET", "live3"}).str, "v");
  EXPECT_EQ(c->Command({"SELECT", "0"}).str, "OK");
  EXPECT_EQ(c->Command({"GET", "live0"}).str, "v");
  EXPECT_EQ(Info(*c, "background_compaction_failures"), 0);
}

}  // namespace
}  // namespace tideskv::test
