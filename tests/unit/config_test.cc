#include "server/config.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

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

TEST(Config, ApplyArgsErrorDoesNotEchoValue) {
  Config c;
  const Status st = c.ApplyArgs({"--port", "7001", "s3cret-password", "x"});
  ASSERT_FALSE(st.ok());
  EXPECT_EQ(st.message(), "expected --option at argument 3");
  EXPECT_EQ(st.message().find("s3cret"), std::string::npos);
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
  EXPECT_EQ(c.Get("*").size(), 14u);
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

TEST(Config, RejectsControlCharactersInStringOptions) {
  Config c;
  // Reject newline in requirepass
  EXPECT_FALSE(c.Set("requirepass", "x\nbind 0.0.0.0", true).ok());
  // Reject other control chars: tab, newline in dir, DEL in bind, SOH in dir, CRLF in logfile
  EXPECT_FALSE(c.Set("requirepass", std::string("x\ty"), true).ok());
  EXPECT_FALSE(c.Set("requirepass", std::string("x\x00y", 3), true).ok());
  EXPECT_FALSE(c.Set("bind", std::string("127.0.0.1\x7f"), false).ok());
  EXPECT_FALSE(c.Set("dir", std::string(".\x01", 2), false).ok());
  EXPECT_FALSE(c.Set("logfile", "/var/log\r\n", false).ok());
}

TEST(Config, RewriteRoundTripsEscapedValues) {
  test::TempDir dir;
  Config c;
  const std::string path = WriteFile(dir, "port 7000\n");
  ASSERT_TRUE(c.LoadFile(path).ok());
  // Set values with special characters that need escaping
  ASSERT_TRUE(c.Set("requirepass", "say \"hi\"", true).ok());
  ASSERT_TRUE(c.Set("logfile", "back\\slash", false).ok());
  ASSERT_TRUE(c.Rewrite().ok());
  Config reloaded;
  ASSERT_TRUE(reloaded.LoadFile(path).ok());
  EXPECT_EQ(reloaded.Snapshot().port, 7000);
  EXPECT_EQ(reloaded.Snapshot().requirepass, "say \"hi\"");
  EXPECT_EQ(reloaded.Snapshot().logfile, "back\\slash");
}

TEST(Config, RewriteHandlesEmptyAndSpacedValues) {
  test::TempDir dir;
  Config c;
  const std::string path = WriteFile(dir, "port 7000\n");
  ASSERT_TRUE(c.LoadFile(path).ok());
  ASSERT_TRUE(c.Set("logfile", "", false).ok());
  ASSERT_TRUE(c.Set("requirepass", "two words", true).ok());
  ASSERT_TRUE(c.Rewrite().ok());
  Config reloaded;
  ASSERT_TRUE(reloaded.LoadFile(path).ok());
  EXPECT_EQ(reloaded.Snapshot().logfile, "");
  EXPECT_EQ(reloaded.Snapshot().requirepass, "two words");
}

TEST(Config, RewritePreservesFilePermissions) {
  test::TempDir dir;
  const std::string path = WriteFile(dir, "port 7000\n");
  // Set permissions to 0600 (owner read/write only)
  const auto expected = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
  std::filesystem::permissions(path, expected, std::filesystem::perm_options::replace);
  Config c;
  ASSERT_TRUE(c.LoadFile(path).ok());
  ASSERT_TRUE(c.Set("requirepass", "test", true).ok());
  ASSERT_TRUE(c.Rewrite().ok());
  // Verify permissions are preserved exactly (not 0644 or other variations)
  const auto perms = std::filesystem::status(path).permissions();
  EXPECT_EQ(perms & std::filesystem::perms::all, expected);
  // Verify no .tmp file remains
  EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));
}

TEST(Config, CompressionOption) {
  Config c;
  EXPECT_EQ(c.Snapshot().compression, "lz4");
  EXPECT_TRUE(c.Set("compression", "ZSTD", false).ok());
  EXPECT_EQ(c.Snapshot().compression, "zstd");
  EXPECT_TRUE(c.Set("compression", "no", false).ok());
  EXPECT_FALSE(c.Set("compression", "snappy", false).ok());
  EXPECT_FALSE(c.Set("compression", "lz4", /*at_runtime=*/true).ok());  // startup-only
}

TEST(Config, RequirepassFromEnvironment) {
  Config c;
  auto env = [](const char* name) -> const char* {
    return std::string_view(name) == "TIDESKV_REQUIREPASS" ? "from-env" : nullptr;
  };
  ASSERT_TRUE(c.ApplyEnv(env).ok());
  EXPECT_EQ(c.Snapshot().requirepass, "from-env");
  ASSERT_TRUE(c.ApplyArgs({"--requirepass", "from-args"}).ok());  // args win
  EXPECT_EQ(c.Snapshot().requirepass, "from-args");
}

TEST(Config, EnvironmentWithoutVariablesChangesNothing) {
  Config c;
  ASSERT_TRUE(c.ApplyEnv([](const char*) -> const char* { return nullptr; }).ok());
  EXPECT_EQ(c.Snapshot().requirepass, "");
}

TEST(Config, EnvRequirepassIsValidated) {
  Config c;
  const Status st = c.ApplyEnv([](const char*) -> const char* { return "bad\npass"; });
  EXPECT_FALSE(st.ok());
  EXPECT_EQ(st.message().find("bad"), std::string::npos) << st.message();  // never echo the secret
  EXPECT_EQ(st.message().find("pass\n"), std::string::npos);
}

TEST(Config, EnvironmentOverridesConfigFile) {
  test::TempDir dir;
  Config c;
  ASSERT_TRUE(c.LoadFile(WriteFile(dir, "requirepass from-file\n")).ok());
  ASSERT_TRUE(c.ApplyEnv([](const char*) -> const char* { return "from-env"; }).ok());
  EXPECT_EQ(c.Snapshot().requirepass, "from-env");
}

TEST(Config, EmptyEnvRequirepassIsIgnored) {  // an unexpanded ${PASS} must never disable auth
  test::TempDir dir;
  Config c;
  ASSERT_TRUE(c.LoadFile(WriteFile(dir, "requirepass x\n")).ok());
  std::vector<std::string> ignored;
  ASSERT_TRUE(c.ApplyEnv([](const char*) -> const char* { return ""; }, &ignored).ok());
  EXPECT_EQ(c.Snapshot().requirepass, "x");
  EXPECT_EQ(ignored, std::vector<std::string>{"TIDESKV_REQUIREPASS"});
}

}  // namespace
}  // namespace tideskv
