#include <gtest/gtest.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>

#include "commands/info.h"
#include "support/command_harness.h"
#include "support/temp_dir.h"

namespace tideskv {
namespace {

using test::CommandHarness;
using test::TempDir;
namespace fs = std::filesystem;

ServerStats SampleStats() {
  ServerStats s;
  s.version = "0.1.0";
  s.pid = 1234;
  s.tcp_port = 6666;
  s.uptime_seconds = 90000;
  s.workers = 4;
  s.maxclients = 10000;
  s.connected_clients = 3;
  s.total_connections = 10;
  s.total_commands = 99;
  s.data_dir = "/data";
  s.sync_mode = "interval";
  return s;
}

TEST(FormatInfo, AllSectionsByDefault) {
  const std::string info = FormatInfo(SampleStats(), {});
  for (const char* needle : {"# Server\r\n", "redis_version:7.2.0\r\n", "tideskv_version:0.1.0\r\n",
                             "tcp_port:6666\r\n", "uptime_in_days:1\r\n", "# Clients\r\n",
                             "connected_clients:3\r\n", "# Stats\r\n", "total_commands_processed:99\r\n",
                             "# Persistence\r\n", "storage_engine:tidesdb\r\n", "# Keyspace\r\n"}) {
    EXPECT_NE(info.find(needle), std::string::npos) << needle;
  }
  EXPECT_EQ(FormatInfo(SampleStats(), {"everything"}), info);
}

TEST(FormatInfo, SelectedSectionsOnly) {
  const std::string info = FormatInfo(SampleStats(), {"CLIENTS"});
  EXPECT_NE(info.find("# Clients"), std::string::npos);
  EXPECT_EQ(info.find("# Server"), std::string::npos);
  EXPECT_EQ(FormatInfo(SampleStats(), {"nosuchsection"}), "");
}

TEST(FormatInfo, AllSectionsAnywhere) {
  // "all" anywhere in the list means all sections
  const std::string all_empty = FormatInfo(SampleStats(), {});
  const std::string all_mixed = FormatInfo(SampleStats(), {"server", "all"});
  EXPECT_EQ(all_empty, all_mixed);
  EXPECT_EQ(all_empty, FormatInfo(SampleStats(), {"default"}));
  EXPECT_EQ(all_empty, FormatInfo(SampleStats(), {"everything"}));
}

TEST(FormatInfo, Deduplication) {
  // Same section requested twice should appear only once
  const std::string once = FormatInfo(SampleStats(), {"server"});
  const std::string twice = FormatInfo(SampleStats(), {"server", "server"});
  EXPECT_EQ(once, twice);
  // Count "# Server" occurrences
  size_t count = 0;
  size_t pos = 0;
  while ((pos = twice.find("# Server", pos)) != std::string::npos) {
    ++count;
    pos += 8;
  }
  EXPECT_EQ(count, 1u);
}

TEST(Info, BulkInResp2VerbatimInResp3) {
  CommandHarness h;
  h.services.stats_ = SampleStats();
  EXPECT_EQ(h.Run({"INFO"}).front(), '$');
  h.Run({"HELLO", "3"});
  const std::string r = h.Run({"INFO", "server"});
  EXPECT_EQ(r.front(), '=');
  EXPECT_NE(r.find("txt:# Server"), std::string::npos);
}

TEST(ConfigCmd, GetAndSet) {
  CommandHarness h;
  EXPECT_EQ(h.Run({"CONFIG", "GET", "maxclients"}), "*2\r\n$10\r\nmaxclients\r\n$5\r\n10000\r\n");
  EXPECT_EQ(h.Run({"CONFIG", "SET", "maxclients", "50"}), "+OK\r\n");
  EXPECT_EQ(h.Run({"CONFIG", "GET", "maxclients"}), "*2\r\n$10\r\nmaxclients\r\n$2\r\n50\r\n");
  h.Run({"HELLO", "3"});
  EXPECT_EQ(h.Run({"CONFIG", "GET", "maxclients"}).rfind("%1\r\n", 0), 0u);
}

TEST(ConfigCmd, SetErrorsAreAtomicAndNamed) {
  CommandHarness h;
  const std::string r = h.Run({"CONFIG", "SET", "maxclients", "5", "port", "1"});
  EXPECT_EQ(r.rfind("-ERR CONFIG SET failed (possibly related to argument 'port') - ", 0), 0u) << r;
  EXPECT_NE(r.find("immutable"), std::string::npos);
  EXPECT_EQ(h.services.config_.Snapshot().maxclients, 10000);
  EXPECT_EQ(h.Run({"CONFIG", "SET", "maxclients"}),
            "-ERR wrong number of arguments for 'config|set' command\r\n");
}

TEST(ConfigCmd, ResetStatRewriteAndUnknown) {
  CommandHarness h;
  EXPECT_EQ(h.Run({"CONFIG", "RESETSTAT"}), "+OK\r\n");
  EXPECT_EQ(h.services.reset_calls_, 1);
  EXPECT_EQ(h.Run({"CONFIG", "REWRITE"}), "-ERR The server is running without a config file\r\n");
  EXPECT_EQ(h.Run({"CONFIG", "NOPE"}),
            "-ERR unknown subcommand or wrong number of arguments for 'NOPE'. Try CONFIG HELP.\r\n");
}

TEST(ConfigCmd, RewritePermissionErrorDoesntExposePathes) {
  // Skip if running as root (permissions are ignored)
  if (::geteuid() == 0) GTEST_SKIP();

  TempDir temp("tideskv-rewrite-test");
  CommandHarness h;

  // Create a config file
  std::string config_path = (temp.path() / "tideskv.conf").string();
  std::ofstream cf(config_path);
  cf << "port 6666\n";
  cf.close();

  // Load the config
  EXPECT_TRUE(h.services.config_.LoadFile(config_path).ok());

  // Make the directory read-only to prevent writing
  fs::permissions(temp.path(), fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);

  // CONFIG REWRITE should fail without exposing the path
  const std::string r = h.Run({"CONFIG", "REWRITE"});
  EXPECT_EQ(r, "-ERR CONFIG REWRITE failed\r\n") << "Got: " << r;
  EXPECT_EQ(r.find("/"), std::string::npos) << "Error message should not contain paths";

  // Restore permissions before TempDir cleanup
  fs::permissions(temp.path(), fs::perms::owner_all, fs::perm_options::replace);
}

TEST(CommandCmd, CountListInfoDocs) {
  CommandHarness h;
  EXPECT_EQ(h.Run({"COMMAND", "COUNT"}), ":" + std::to_string(h.services.registry().size()) + "\r\n");
  EXPECT_NE(h.Run({"COMMAND", "LIST"}).find("$4\r\nping\r\n"), std::string::npos);
  EXPECT_EQ(h.Run({"COMMAND", "INFO", "ping"}),
            "*1\r\n*6\r\n$4\r\nping\r\n:-1\r\n*1\r\n+fast\r\n:0\r\n:0\r\n:0\r\n");
  EXPECT_EQ(h.Run({"COMMAND", "INFO", "nope"}), "*1\r\n*-1\r\n");
  EXPECT_EQ(h.Run({"COMMAND", "DOCS"}), "*0\r\n");
  EXPECT_EQ(h.Run({"COMMAND"}).rfind("*" + std::to_string(h.services.registry().size()) + "\r\n", 0), 0u);
}

TEST(Shutdown, RequestsShutdownWithoutReply) {
  CommandHarness h;
  EXPECT_EQ(h.Run({"SHUTDOWN", "NOSAVE"}), "");
  EXPECT_TRUE(h.services.shutdown_requested_);
  EXPECT_TRUE(h.session.close_after_reply);
}

TEST(Shutdown, AbortAndBadOption) {
  CommandHarness h;
  EXPECT_EQ(h.Run({"SHUTDOWN", "ABORT"}), "-ERR No shutdown in progress.\r\n");
  EXPECT_EQ(h.Run({"SHUTDOWN", "BOGUS"}), "-ERR syntax error\r\n");
  EXPECT_FALSE(h.services.shutdown_requested_);
}

}  // namespace
}  // namespace tideskv
