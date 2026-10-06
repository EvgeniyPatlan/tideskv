#include "server/config.h"

#include <gtest/gtest.h>

#include <fstream>

#include "support/temp_dir.h"

namespace tideskv {
namespace {

std::string WriteFile(const test::TempDir& dir, const std::string& content) {
  const std::string path = (dir.path() / "tideskv.conf").string();
  std::ofstream(path) << content;
  return path;
}

TEST(Config, DefaultsMatchSpec) {
  const ConfigValues v = Config().Snapshot();
  EXPECT_EQ(v.bind, "127.0.0.1");
  EXPECT_EQ(v.port, 6666);
  EXPECT_EQ(v.sync_mode, "interval");
  EXPECT_EQ(v.block_cache_size, 4ULL << 30);
  EXPECT_EQ(v.memtable_size, 64ULL << 20);
  EXPECT_EQ(v.maxclients, 10000);
  EXPECT_EQ(v.client_query_buffer_limit, 1ULL << 30);
  EXPECT_TRUE(v.requirepass.empty());
}

TEST(Config, LoadFileParsesCommentsQuotesAndUnits) {
  test::TempDir dir;
  Config c;
  ASSERT_TRUE(c.LoadFile(WriteFile(dir,
                                   "# comment\n\nport 7000\nrequirepass \"my secret\"\n"
                                   "BLOCK-CACHE-SIZE 1gb\n  sync-mode full  \n"))
                  .ok());
  const ConfigValues v = c.Snapshot();
  EXPECT_EQ(v.port, 7000);
  EXPECT_EQ(v.requirepass, "my secret");
  EXPECT_EQ(v.block_cache_size, 1ULL << 30);
  EXPECT_EQ(v.sync_mode, "full");
}

TEST(Config, LoadFileReportsLineOfUnknownOption) {
  test::TempDir dir;
  Config c;
  const Status st = c.LoadFile(WriteFile(dir, "port 7000\n# x\nnope 1\n"));
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("line 3"), std::string::npos) << st.message();
  EXPECT_NE(st.message().find("unknown option 'nope'"), std::string::npos) << st.message();
  EXPECT_EQ(c.Snapshot().port, 6666);  // nothing applied on failure
}

TEST(Config, LoadFileRejectsBadValueAndMissingFile) {
  test::TempDir dir;
  Config c;
  EXPECT_FALSE(c.LoadFile(WriteFile(dir, "port 99999\n")).ok());
  EXPECT_FALSE(c.LoadFile(WriteFile(dir, "port\n")).ok());
  EXPECT_EQ(c.LoadFile((dir.path() / "missing.conf").string()).code(), StatusCode::kIOError);
}

TEST(Config, ApplyArgs) {
  Config c;
  ASSERT_TRUE(c.ApplyArgs({"--port", "7001", "--dir", "/var/lib/tideskv"}).ok());
  EXPECT_EQ(c.Snapshot().port, 7001);
  EXPECT_EQ(c.Snapshot().dir, "/var/lib/tideskv");
  EXPECT_FALSE(c.ApplyArgs({"--port"}).ok());
  EXPECT_FALSE(c.ApplyArgs({"port", "1"}).ok());
}

TEST(Config, RuntimeSetRejectsImmutable) {
  Config c;
  const Status st = c.Set("port", "7000", /*at_runtime=*/true);
  ASSERT_FALSE(st.ok());
  EXPECT_NE(st.message().find("immutable"), std::string::npos);
  EXPECT_TRUE(c.Set("maxclients", "5", true).ok());
  EXPECT_EQ(c.Snapshot().maxclients, 5);
}

TEST(Config, ValidatesValues) {
  Config c;
  for (const auto& [name, value] :
       std::vector<std::pair<std::string, std::string>>{{"port", "0"},
                                                        {"port", "abc"},
                                                        {"sync-mode", "sometimes"},
                                                        {"loglevel", "loud"},
                                                        {"maxclients", "-1"},
                                                        {"workers", "2000"},
                                                        {"block-cache-size", "lots"},
                                                        {"bind", ""},
                                                        {"client-query-buffer-limit", "1k"}}) {
    EXPECT_FALSE(c.Set(name, value, false).ok()) << name << "=" << value;
  }
}

TEST(Config, SetManyIsAtomic) {
  Config c;
  std::string failed;
  const Status st = c.SetMany({{"maxclients", "5"}, {"port", "1"}}, true, &failed);
  ASSERT_FALSE(st.ok());
  EXPECT_EQ(failed, "port");
  EXPECT_EQ(c.Snapshot().maxclients, 10000);
}

TEST(Config, GetUsesCaseInsensitiveGlob) {
  Config c;
  EXPECT_EQ(c.Get("port"), (std::vector<std::pair<std::string, std::string>>{{"port", "6666"}}));
  EXPECT_EQ(c.Get("PORT").size(), 1u);
  EXPECT_EQ(c.Get("max*"), (std::vector<std::pair<std::string, std::string>>{{"maxclients", "10000"}}));
  EXPECT_EQ(c.Get("*").size(), 13u);
}

TEST(Config, RewritePersistsRuntimeChanges) {
  test::TempDir dir;
  Config c;
  const std::string path = WriteFile(dir, "port 7000\n");
  ASSERT_TRUE(c.LoadFile(path).ok());
  ASSERT_TRUE(c.Set("requirepass", "with space", true).ok());
  ASSERT_TRUE(c.Rewrite().ok());
  Config reloaded;
  ASSERT_TRUE(reloaded.LoadFile(path).ok());
  EXPECT_EQ(reloaded.Snapshot().port, 7000);
  EXPECT_EQ(reloaded.Snapshot().requirepass, "with space");
}

TEST(Config, RewriteWithoutFileFails) { EXPECT_FALSE(Config().Rewrite().ok()); }

}  // namespace
}  // namespace tideskv
