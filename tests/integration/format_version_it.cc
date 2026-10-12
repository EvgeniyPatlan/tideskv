#include <gtest/gtest.h>

#include "storage/tides_storage.h"
#include "support/temp_dir.h"
#include "support/test_server.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace tideskv::test {
namespace {

TEST(FormatVersionIT, UnknownFormatVersionRefusesToStart) {  // Review Focus #5
  TempDir dir("tideskv-fmt");
  {
    StorageOptions o;
    o.path = (dir.path() / "db").string();
    o.sync_mode = SyncMode::kNone;
    auto s = TidesStorage::Open(o);
    ASSERT_TRUE(s.ok()) << s.status().ToString();
    auto txn = (*s)->BeginWrite();
    ASSERT_TRUE((*txn)->Put(ColumnFamily::kSys, "format_version", "99").ok());
    ASSERT_TRUE((*txn)->Commit().ok());
  }
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

}  // namespace
}  // namespace tideskv::test
