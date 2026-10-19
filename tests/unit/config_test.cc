#include "server/config.h"

#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
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
  EXPECT_EQ(v.l1_file_count_trigger, 8);
  EXPECT_EQ(v.l0_stall_threshold, 64);
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
                                                        {"client-query-buffer-limit", "1k"},
                                                        {"l1-file-count-trigger", "0"},
                                                        {"l1-file-count-trigger", "1025"},
                                                        {"l0-stall-threshold", "1"},
                                                        {"l0-stall-threshold", "1025"}}) {
    EXPECT_FALSE(c.Set(name, value, false).ok()) << name << "=" << value;
  }
}

// M8: the TidesDB compaction and backpressure knobs; set at startup only (TidesDB reads them at open).
TEST(Config, StorageTuningIsParsedAndStartupOnly) {
  Config c;
  ASSERT_TRUE(c.Set("l1-file-count-trigger", "16", false).ok());
  ASSERT_TRUE(c.Set("l0-stall-threshold", "96", false).ok());
  EXPECT_EQ(c.Snapshot().l1_file_count_trigger, 16);
  EXPECT_EQ(c.Snapshot().l0_stall_threshold, 96);
  EXPECT_FALSE(c.Set("l1-file-count-trigger", "4", true).ok());
  EXPECT_FALSE(c.Set("l0-stall-threshold", "16", true).ok());
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
  EXPECT_EQ(c.Get("*").size(), 44u);
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

TEST(Config, CompactionCollectionDeletes) {
  Config c;
  EXPECT_EQ(c.Snapshot().compaction_collection_deletes, 1000u);
  EXPECT_TRUE(c.Set("compaction-collection-deletes", "0", true).ok());  // runtime-mutable; 0 = never
  EXPECT_EQ(c.Snapshot().compaction_collection_deletes, 0u);
  EXPECT_FALSE(c.Set("compaction-collection-deletes", "-1", true).ok());
}

std::string GetOne(const Config& c, std::string_view name) {
  const auto got = c.Get(name);
  return got.size() == 1 ? got[0].second : "<missing>";
}

TEST(ConfigRepl, Defaults) {
  const ConfigValues v = Config().Snapshot();
  EXPECT_EQ(v.replicaof, "");
  EXPECT_EQ(v.masterauth, "");
  EXPECT_EQ(v.masteruser, "");
  EXPECT_TRUE(v.replica_read_only);
  EXPECT_EQ(v.repl_backlog_size, 1ULL << 30);
  EXPECT_EQ(v.repl_backlog_ttl, 3600);
  EXPECT_EQ(v.repl_timeout, 60);
  EXPECT_EQ(v.replica_announce_ip, "");
  EXPECT_EQ(v.replica_announce_port, 0);
  EXPECT_EQ(v.replica_priority, 100);
  Config c;
  EXPECT_EQ(GetOne(c, "replica-read-only"), "yes");
  EXPECT_EQ(GetOne(c, "repl-backlog-size"), "1073741824");
}

TEST(ConfigRepl, MutableKeysRoundTrip) {
  Config c;
  ASSERT_TRUE(c.Set("masterauth", "s3cret", true).ok());
  ASSERT_TRUE(c.Set("masteruser", "repl", true).ok());
  ASSERT_TRUE(c.Set("repl-backlog-size", "2mb", true).ok());
  ASSERT_TRUE(c.Set("repl-backlog-ttl", "0", true).ok());
  ASSERT_TRUE(c.Set("repl-timeout", "5", true).ok());
  ASSERT_TRUE(c.Set("replica-announce-ip", "10.0.0.7", true).ok());
  ASSERT_TRUE(c.Set("replica-announce-port", "7001", true).ok());
  ASSERT_TRUE(c.Set("replica-priority", "0", true).ok());
  ASSERT_TRUE(c.Set("replica-read-only", "YES", true).ok());
  const ConfigValues v = c.Snapshot();
  EXPECT_EQ(v.masterauth, "s3cret");
  EXPECT_EQ(v.masteruser, "repl");
  EXPECT_EQ(v.repl_backlog_size, 2097152u);
  EXPECT_EQ(v.repl_backlog_ttl, 0);
  EXPECT_EQ(v.repl_timeout, 5);
  EXPECT_EQ(v.replica_announce_ip, "10.0.0.7");
  EXPECT_EQ(v.replica_announce_port, 7001);
  EXPECT_EQ(v.replica_priority, 0);
  EXPECT_EQ(GetOne(c, "masterauth"), "s3cret");  // clear text, like requirepass
  EXPECT_EQ(GetOne(c, "repl-backlog-size"), "2097152");
}

TEST(ConfigRepl, BoundsErrorTexts) {
  Config c;
  EXPECT_EQ(c.Set("repl-backlog-size", "0", true).message(),
            "argument must be between 1 and 9223372036854775807 inclusive");
  EXPECT_FALSE(c.Set("repl-backlog-size", "junk", true).ok());
  EXPECT_EQ(c.Set("repl-timeout", "0", true).message(),
            "argument must be between 1 and 2147483647 inclusive");
  EXPECT_FALSE(c.Set("repl-timeout", "2147483648", true).ok());
  EXPECT_EQ(c.Set("repl-backlog-ttl", "-1", true).message(),
            "argument must be between 0 and 9223372036854775807 inclusive");
  EXPECT_EQ(c.Set("replica-announce-port", "65536", true).message(),
            "argument must be between 0 and 65535 inclusive");
  EXPECT_EQ(c.Set("replica-priority", "-1", true).message(),
            "argument must be between 0 and 2147483647 inclusive");
  EXPECT_EQ(c.Set("replica-read-only", "maybe", true).message(), "argument must be 'yes' or 'no'");
  EXPECT_FALSE(c.Set("masterauth", "a\nb", true).ok());
  EXPECT_EQ(c.Snapshot().repl_timeout, 60);  // failures change nothing
}

TEST(ConfigRepl, WritableReplicasRejected) {
  Config c;
  const Status st = c.Set("replica-read-only", "no", true);
  EXPECT_EQ(st.code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(st.message(), "writable replicas are not supported");
  EXPECT_TRUE(c.Snapshot().replica_read_only);
}

TEST(ConfigRepl, AliasesResolveToSameValue) {
  Config c;
  ASSERT_TRUE(c.Set("slave-read-only", "yes", true).ok());
  ASSERT_TRUE(c.Set("slave-announce-ip", "1.2.3.4", true).ok());
  ASSERT_TRUE(c.Set("slave-announce-port", "9", true).ok());
  ASSERT_TRUE(c.Set("slave-priority", "7", true).ok());
  EXPECT_EQ(c.Snapshot().replica_announce_ip, "1.2.3.4");
  EXPECT_EQ(c.Snapshot().replica_announce_port, 9);
  EXPECT_EQ(c.Snapshot().replica_priority, 7);
  EXPECT_EQ(GetOne(c, "slave-announce-ip"), "1.2.3.4");  // asked name is the returned name
  EXPECT_EQ(GetOne(c, "slave-priority"), "7");
  EXPECT_EQ(GetOne(c, "slave-read-only"), "yes");
  EXPECT_EQ(c.Set("slave-read-only", "no", true).message(), "writable replicas are not supported");
  ASSERT_TRUE(c.Set("slaveof", "h 1", false).ok());
  EXPECT_EQ(c.Snapshot().replicaof, "h 1");
  EXPECT_EQ(GetOne(c, "slaveof"), "h 1");
}

TEST(ConfigRepl, ReplicaofParsing) {
  Config c;
  ASSERT_TRUE(c.Set("replicaof", "example.org 6379", false).ok());
  EXPECT_EQ(c.Snapshot().replicaof, "example.org 6379");
  EXPECT_EQ(GetOne(c, "replicaof"), "example.org 6379");
  ASSERT_TRUE(c.Set("replicaof", "NO ONE", false).ok());
  EXPECT_EQ(c.Snapshot().replicaof, "");
  EXPECT_EQ(c.Set("replicaof", "host 65536", false).message(), "Invalid master port");
  EXPECT_EQ(c.Set("replicaof", "host -1", false).message(), "Invalid master port");
  EXPECT_EQ(c.Set("replicaof", "host 12x", false).message(), "Invalid master port");
  EXPECT_EQ(c.Set("replicaof", "justhost", false).message(), "wrong number of arguments");
  EXPECT_EQ(c.Set("replicaof", "a b c", false).message(), "wrong number of arguments");
  EXPECT_EQ(c.Snapshot().replicaof, "");
}

TEST(ConfigRepl, ReplicaofIsStartupOnly) {
  Config c;
  const Status st = c.Set("replicaof", "no one", true);
  EXPECT_EQ(st.message(), "can't set immutable config");
  EXPECT_EQ(c.Set("slaveof", "h 1", true).message(), "can't set immutable config");
  ASSERT_TRUE(c.ApplyArgs({"--replicaof", "10.0.0.1 7000"}).ok());
  EXPECT_EQ(c.Snapshot().replicaof, "10.0.0.1 7000");
}

TEST(ConfigRepl, ReplicaofFromFileAndRewrite) {
  test::TempDir dir;
  Config c;
  const std::string path = WriteFile(dir, "replicaof 10.0.0.1 7000\nmasterauth \"p w\"\n");
  ASSERT_TRUE(c.LoadFile(path).ok());
  EXPECT_EQ(c.Snapshot().replicaof, "10.0.0.1 7000");
  ASSERT_TRUE(c.Rewrite().ok());
  std::ifstream in(path);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_NE(text.find("\nreplicaof 10.0.0.1 7000\n"), std::string::npos) << text;
  EXPECT_EQ(text.find("slaveof"), std::string::npos) << text;  // aliases are not rewritten
  EXPECT_EQ(text.find("slave-"), std::string::npos) << text;
  Config reloaded;
  ASSERT_TRUE(reloaded.LoadFile(path).ok());
  EXPECT_EQ(reloaded.Snapshot().replicaof, "10.0.0.1 7000");
  EXPECT_EQ(reloaded.Snapshot().masterauth, "p w");
  EXPECT_EQ(reloaded.Snapshot().repl_backlog_size, 1ULL << 30);
}

TEST(ConfigRepl, RewriteOmitsReplicaofForMaster) {
  test::TempDir dir;
  Config c;
  const std::string path = WriteFile(dir, "port 7000\n");
  ASSERT_TRUE(c.LoadFile(path).ok());
  ASSERT_TRUE(c.Rewrite().ok());
  std::ifstream in(path);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(text.find("replicaof"), std::string::npos) << text;
  EXPECT_NE(text.find("repl-timeout 60"), std::string::npos) << text;
}

TEST(ConfigRepl, MasterauthNeverLogged) {
  constexpr std::string_view kSecret = "hunter2-masterauth";
  std::ostringstream captured;
  auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(captured);
  auto logger = std::make_shared<spdlog::logger>("capture", sink);
  logger->set_level(spdlog::level::trace);
  auto previous = spdlog::default_logger();
  spdlog::set_default_logger(logger);
  test::TempDir dir;
  Config c;
  const std::string path = WriteFile(dir, "masterauth " + std::string(kSecret) + "\n");
  Status results[] = {c.LoadFile(path), c.Set("masterauth", kSecret, true),
                      c.ApplyArgs({"--masterauth", std::string(kSecret)}), c.Rewrite(),
                      c.Set("masterauth", std::string(kSecret) + "\n", true)};
  spdlog::set_default_logger(previous);
  EXPECT_TRUE(results[0].ok() && results[1].ok() && results[2].ok() && results[3].ok());
  EXPECT_FALSE(results[4].ok());
  EXPECT_EQ(results[4].message().find(kSecret), std::string::npos) << results[4].message();
  EXPECT_EQ(captured.str().find(kSecret), std::string::npos) << captured.str();
}

TEST(ConfigRepl, M6bDefaults) {
  const ConfigValues v = Config().Snapshot();
  EXPECT_TRUE(v.replica_serve_stale_data);
  EXPECT_EQ(v.min_replicas_to_write, 0);
  EXPECT_EQ(v.min_replicas_max_lag, 10);
  EXPECT_EQ(v.key_load_delay, 0);
  EXPECT_EQ(v.rdb_key_save_delay, 0);
  Config c;
  EXPECT_EQ(GetOne(c, "replica-serve-stale-data"), "yes");
  EXPECT_EQ(GetOne(c, "min-replicas-max-lag"), "10");
}

TEST(ConfigRepl, M6bKeysRoundTripWithAliases) {
  Config c;
  ASSERT_TRUE(c.Set("slave-serve-stale-data", "no", true).ok());
  ASSERT_TRUE(c.Set("min-slaves-to-write", "2", true).ok());
  ASSERT_TRUE(c.Set("min-slaves-max-lag", "3", true).ok());
  ASSERT_TRUE(c.Set("key-load-delay", "-10", true).ok());
  ASSERT_TRUE(c.Set("rdb-key-save-delay", "250", true).ok());
  const ConfigValues v = c.Snapshot();
  EXPECT_FALSE(v.replica_serve_stale_data);
  EXPECT_EQ(v.min_replicas_to_write, 2);
  EXPECT_EQ(v.min_replicas_max_lag, 3);
  EXPECT_EQ(v.key_load_delay, -10);
  EXPECT_EQ(v.rdb_key_save_delay, 250);
  EXPECT_EQ(GetOne(c, "replica-serve-stale-data"), "no");
  EXPECT_EQ(GetOne(c, "slave-serve-stale-data"), "no");
  EXPECT_EQ(GetOne(c, "min-replicas-to-write"), "2");
}

TEST(ConfigRepl, M6bBoundsErrorTexts) {  // live redis:7.2.5
  Config c;
  EXPECT_EQ(c.Set("replica-serve-stale-data", "maybe", true).message(), "argument must be 'yes' or 'no'");
  EXPECT_EQ(c.Set("min-replicas-max-lag", "-1", true).message(),
            "argument must be between 0 and 2147483647 inclusive");
  EXPECT_EQ(c.Set("min-replicas-to-write", "2147483648", true).message(),
            "argument must be between 0 and 2147483647 inclusive");
  EXPECT_EQ(c.Set("key-load-delay", "2147483648", true).message(),
            "argument must be between -2147483648 and 2147483647 inclusive");
}

TEST(ConfigRepl, HiddenOptionsMatchOnlyByExactName) {  // Redis HIDDEN_CONFIG, live redis:7.2.5
  Config c;
  ASSERT_TRUE(c.Set("key-load-delay", "5", true).ok());
  EXPECT_EQ(c.Get("key-load-delay"), (Config::Pairs{{"key-load-delay", "5"}}));
  EXPECT_EQ(c.Get("KEY-LOAD-DELAY"), (Config::Pairs{{"key-load-delay", "5"}}));
  EXPECT_TRUE(c.Get("key-load*").empty());
  for (const auto& [name, value] : c.Get("*")) {
    EXPECT_NE(name, "key-load-delay");
    EXPECT_NE(name, "rdb-key-save-delay");
  }
}

TEST(ConfigRepl, HiddenOptionsAreNeverRewritten) {
  test::TempDir dir;
  Config c;
  const std::string path = WriteFile(dir, "port 7000\n");
  ASSERT_TRUE(c.LoadFile(path).ok());
  ASSERT_TRUE(c.Set("rdb-key-save-delay", "100", true).ok());
  ASSERT_TRUE(c.Rewrite().ok());
  std::ifstream in(path);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(text.find("rdb-key-save-delay"), std::string::npos) << text;
  EXPECT_EQ(text.find("key-load-delay"), std::string::npos) << text;
  EXPECT_NE(text.find("\nreplica-serve-stale-data yes\n"), std::string::npos) << text;
}

TEST(ConfigRepl, ChangeHookSeesEverySuccessfulSetAndNoFailedOne) {
  Config c;
  std::vector<ConfigValues> seen;
  c.SetOnChange([&](const ConfigValues& v) { seen.push_back(v); });
  ASSERT_TRUE(c.Set("min-replicas-to-write", "1", true).ok());
  ASSERT_FALSE(c.Set("min-replicas-to-write", "x", true).ok());
  ASSERT_TRUE(
      c.SetMany({{"replica-serve-stale-data", "no"}, {"min-replicas-max-lag", "4"}}, true, nullptr).ok());
  ASSERT_EQ(seen.size(), 2u);
  EXPECT_EQ(seen[0].min_replicas_to_write, 1);
  EXPECT_FALSE(seen[1].replica_serve_stale_data);
  EXPECT_EQ(seen[1].min_replicas_max_lag, 4);
}

TEST(ConfigCluster, ClusterEnabledIsAStartupOnlyBoolean) {  // M7a D19
  Config c;
  EXPECT_FALSE(c.Snapshot().cluster_enabled);
  EXPECT_EQ(GetOne(c, "cluster-enabled"), "no");
  ASSERT_TRUE(c.ApplyArgs({"--cluster-enabled", "yes"}).ok());
  EXPECT_TRUE(c.Snapshot().cluster_enabled);
  EXPECT_EQ(GetOne(c, "cluster-enabled"), "yes");
  EXPECT_EQ(c.Set("cluster-enabled", "no", /*at_runtime=*/true).message(), "can't set immutable config");
  EXPECT_EQ(c.Set("cluster-enabled", "maybe", /*at_runtime=*/false).message(),
            "argument must be 'yes' or 'no'");
}

TEST(ConfigMigrate, TheMigrateKnobsAreRuntimeAndTheDeprecatedOnesStartupOnly) {  // M7b D17, KM §9
  Config c;
  EXPECT_EQ(GetOne(c, "migrate-sequence-gap"), "10000");
  EXPECT_EQ(GetOne(c, "migrate-batch-size-kb"), "16");
  EXPECT_EQ(GetOne(c, "migrate-batch-rate-limit-mb"), "16");
  ASSERT_TRUE(c.Set("migrate-batch-size-kb", "1", true).ok());
  ASSERT_TRUE(c.Set("migrate-batch-rate-limit-mb", "1", true).ok());
  EXPECT_EQ(c.Snapshot().migrate_batch_size_kb, 1);
  EXPECT_EQ(c.Set("migrate-sequence-gap", "0", true).message(),
            "argument must be between 1 and 2147483647 inclusive");
  ASSERT_TRUE(c.Set("migrate-delay-ms", "250", true).ok());  // hidden (D25)
  EXPECT_EQ(c.Get("migrate-delay-ms"), (Config::Pairs{{"migrate-delay-ms", "250"}}));
  for (const auto& [name, value] : c.Get("*")) EXPECT_NE(name, "migrate-delay-ms");
  ASSERT_TRUE(c.ApplyArgs({"--migrate-type", "redis-command"}).ok());  // accepted with a warning, no effect
  test::TempDir dir;
  ASSERT_TRUE(c.LoadFile(WriteFile(dir, "migrate-speed 4096\nmigrate-pipeline-size 16\n")).ok());
  EXPECT_EQ(c.Set("migrate-type", "redis-command", true).message(), "unknown option 'migrate-type'");
  EXPECT_TRUE(c.Get("migrate-type").empty());
}

}  // namespace
}  // namespace tideskv
