#include <gtest/gtest.h>

#include "common/crc16.h"
#include "encoding/key_codec.h"
#include "encoding/meta_codec.h"
#include "storage/tides_storage.h"
#include "support/temp_dir.h"
#include "support/test_server.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace tideskv::test {
namespace {

std::unique_ptr<TidesStorage> OpenStore(const TempDir& dir) {
  StorageOptions o;
  o.path = (dir.path() / "db").string();
  o.sync_mode = SyncMode::kNone;
  auto s = TidesStorage::Open(o);
  EXPECT_TRUE(s.ok()) << s.status().ToString();
  return s.ok() ? std::move(s).value() : nullptr;
}

std::string FormatVersionOf(const TempDir& dir) {
  auto s = OpenStore(dir);
  if (s == nullptr) return "<cannot open>";
  auto view = s->BeginRead();
  auto v = (*view)->Get(ColumnFamily::kSys, "format_version");
  return v.ok() ? *v : "<" + v.status().ToString() + ">";
}

void PutAll(const TempDir& dir, const std::vector<std::tuple<ColumnFamily, std::string, std::string>>& puts) {
  auto s = OpenStore(dir);
  ASSERT_NE(s, nullptr);
  auto txn = s->BeginWrite();
  for (const auto& [cf, key, value] : puts) ASSERT_TRUE((*txn)->Put(cf, key, value).ok());
  ASSERT_TRUE((*txn)->Commit().ok());
}

std::vector<std::string> Cluster() { return {"--cluster-enabled", "yes"}; }

TEST(FormatVersionIT, UnknownFormatVersionRefusesToStart) {  // Review Focus #5
  TempDir dir("tideskv-fmt");
  PutAll(dir, {{ColumnFamily::kSys, "format_version", "99"}});
  TestServer server(TIDESKV_SERVER_BIN, {.dir = dir.str(), .wait_ready = false});
  EXPECT_EQ(server.Wait(), 2);
  EXPECT_NE(server.Stderr().find("format version 99"), std::string::npos) << server.Stderr();
}

TEST(FormatVersionIT, FreshDirStartsAndRestarts) {
  TempDir dir("tideskv-fmt");
  for (int i = 0; i < 2; ++i) {
    TestServer server(TIDESKV_SERVER_BIN, {.dir = dir.str()});
    auto c = server.Connect();
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->Command({"PING"}).str, "PONG");
    EXPECT_EQ(server.Terminate(), 0);
  }
}

TEST(FormatVersionIT, AFreshDirTakesTheLayoutOfTheClusterMode) {
  TempDir standalone("tideskv-fmt");
  {
    TestServer server(TIDESKV_SERVER_BIN, {.dir = standalone.str()});
    auto c = server.Connect();
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->Command({"SET", "k", "v"}).str, "OK");
    EXPECT_EQ(server.Terminate(), 0);
  }
  EXPECT_EQ(FormatVersionOf(standalone), "2");
  {
    auto s = OpenStore(standalone);  // the meta key has no slot: [db][user_key]
    auto view = s->BeginRead();
    EXPECT_TRUE((*view)->Get(ColumnFamily::kMeta, std::string("\0k", 2)).ok());
  }
  TempDir cluster("tideskv-fmt");
  {
    TestServer server(TIDESKV_SERVER_BIN, {.extra_args = Cluster(), .dir = cluster.str()});
    EXPECT_EQ(server.Terminate(), 0);
  }
  EXPECT_EQ(FormatVersionOf(cluster), "1");
}

// A dir an older TidesKV wrote (format 1, every key with its slot) keeps working in standalone mode.
TEST(FormatVersionIT, ALegacySlottedDirIsServedAsBefore) {
  TempDir dir("tideskv-fmt");
  encoding::Metadata m;
  m.type = encoding::ValueType::kString;
  std::string slotted_key(1, '\0');
  slotted_key += static_cast<char>(KeySlot("legacy") >> 8);
  slotted_key += static_cast<char>(KeySlot("legacy") & 0xff);
  slotted_key += "legacy";
  PutAll(dir, {{ColumnFamily::kSys, "format_version", "1"},
               {ColumnFamily::kMeta, slotted_key, encoding::EncodeMetadata(m, "old value")}});
  TestServer server(TIDESKV_SERVER_BIN, {.dir = dir.str()});
  auto c = server.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"GET", "legacy"}).str, "old value");
  EXPECT_EQ(c->Command({"SET", "new", "v"}).str, "OK");
  EXPECT_EQ(c->Command({"KEYS", "*"}).elements.size(), 2u);
  EXPECT_NE(server.Stderr().find("slotted key layout (format 1)"), std::string::npos) << server.Stderr();
  EXPECT_EQ(server.Terminate(), 0);
  EXPECT_EQ(FormatVersionOf(dir), "1");
}

TEST(FormatVersionIT, ASlotlessDirWithKeysRefusesClusterMode) {
  TempDir dir("tideskv-fmt");
  {
    TestServer server(TIDESKV_SERVER_BIN, {.dir = dir.str()});
    auto c = server.Connect();
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->Command({"SET", "k", "v"}).str, "OK");
    EXPECT_EQ(server.Terminate(), 0);
  }
  TestServer server(TIDESKV_SERVER_BIN, {.extra_args = Cluster(), .dir = dir.str(), .wait_ready = false});
  EXPECT_EQ(server.Wait(), 2);
  EXPECT_NE(
      server.Stderr().find("this data directory uses the standalone key layout (format 2); cluster mode "
                           "needs format 1"),
      std::string::npos)
      << server.Stderr();
  EXPECT_EQ(FormatVersionOf(dir), "2");
}

TEST(FormatVersionIT, AnEmptiedDirSwitchesLayoutAtBoot) {
  TempDir dir("tideskv-fmt");
  {
    TestServer server(TIDESKV_SERVER_BIN, {.dir = dir.str()});
    auto c = server.Connect();
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->Command({"SET", "k", "v"}).str, "OK");
    EXPECT_EQ(c->Command({"DEL", "k"}).integer, 1);  // the log is dormant: no op is retained
    EXPECT_EQ(server.Terminate(), 0);
  }
  ASSERT_EQ(FormatVersionOf(dir), "2");
  {
    TestServer server(TIDESKV_SERVER_BIN, {.extra_args = Cluster(), .dir = dir.str()});
    EXPECT_NE(server.Stderr().find("it now uses the slotted key layout (format 1)"), std::string::npos)
        << server.Stderr();
    EXPECT_EQ(server.Terminate(), 0);
  }
  EXPECT_EQ(FormatVersionOf(dir), "1");
  {
    TestServer server(TIDESKV_SERVER_BIN, {.dir = dir.str()});
    EXPECT_EQ(server.Terminate(), 0);
  }
  EXPECT_EQ(FormatVersionOf(dir), "2");
}

}  // namespace
}  // namespace tideskv::test
