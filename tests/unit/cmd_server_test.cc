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
  s.storage_compression = "lz4";
  s.storage_keys = 77;
  s.storage_tombstones = 12;
  s.storage_flushes = 3;
  s.storage_compactions = 4;
  s.expired_keys = 9;
  return s;
}

TEST(FormatInfo, AllSectionsByDefault) {
  const std::string info = FormatInfo(SampleStats(), {});
  for (const char* needle : {"# Server\r\n",
                             "redis_version:7.2.0\r\n",
                             "tideskv_version:0.1.0\r\n",
                             "tcp_port:6666\r\n",
                             "uptime_in_days:1\r\n",
                             "# Clients\r\n",
                             "connected_clients:3\r\n",
                             "# Memory\r\n",
                             "used_memory_lua:",
                             "number_of_cached_scripts:",
                             "# Stats\r\n",
                             "total_commands_processed:99\r\n",
                             "# Persistence\r\n",
                             "storage_engine:tidesdb\r\n",
                             "compression:lz4\r\n",
                             "sstable_keys:77\r\n",
                             "tombstones:12\r\n",
                             "flushes:3\r\n",
                             "compactions:4\r\n",
                             "expired_keys:9\r\n",
                             "# Keyspace\r\n"}) {
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

TEST(InfoBlocking, ClientsSectionHasBlockedFields) {
  test::CommandHarness h;
  h.services.stats_.blocked_clients = 2;
  h.services.stats_.blocking_keys = 3;
  const std::string info = h.Run({"INFO", "clients"});
  EXPECT_NE(info.find("blocked_clients:2\r\n"), std::string::npos);
  EXPECT_NE(info.find("total_blocking_keys:3\r\n"), std::string::npos);
}

// ---- INFO replication (M6a D16) ----

const std::string kId(40, 'a');
const std::string kZeros(40, '0');

ReplicationStats MasterRepl() {
  ReplicationStats r;
  r.replid = kId;
  r.master_offset = r.applied_seq = r.read_seq = 15;
  r.backlog_size_config = 1073741824;
  r.first_retained = 2;
  r.histlen = 14;
  return r;
}

std::string Repl(const ReplicationStats& r) {
  ServerStats s = SampleStats();
  s.repl = r;
  return FormatInfo(s, {"replication"});
}

TEST(InfoReplication, MasterWithoutReplicasIsExact) {
  EXPECT_EQ(Repl(MasterRepl()),
            "# Replication\r\nrole:master\r\nconnected_slaves:0\r\n"
            "master_failover_state:no-failover\r\nmaster_replid:" +
                kId + "\r\nmaster_replid2:" + kZeros +
                "\r\nmaster_repl_offset:15\r\nsecond_repl_offset:-1\r\nrepl_backlog_active:1\r\n"
                "repl_backlog_size:1073741824\r\nrepl_backlog_first_byte_offset:2\r\n"
                "repl_backlog_histlen:14\r\n");
}

TEST(InfoReplication, EmptyLogHasZeroHistlen) {
  ReplicationStats r = MasterRepl();
  r.master_offset = 0;
  r.first_retained = 1;
  r.histlen = 0;
  const std::string info = Repl(r);
  EXPECT_NE(info.find("master_repl_offset:0\r\n"), std::string::npos);
  EXPECT_NE(info.find("repl_backlog_histlen:0\r\n"), std::string::npos);
}

TEST(InfoReplication, MasterListsReplicas) {
  ReplicationStats r = MasterRepl();
  r.replicas = {{"10.0.0.2", 7001, 15, 0}, {"10.0.0.3", 7002, 12, 2}};
  const std::string info = Repl(r);
  EXPECT_NE(info.find("role:master\r\nconnected_slaves:2\r\n"
                      "slave0:ip=10.0.0.2,port=7001,state=online,offset=15,lag=0\r\n"
                      "slave1:ip=10.0.0.3,port=7002,state=online,offset=12,lag=2\r\n"
                      "master_failover_state:no-failover\r\n"),
            std::string::npos)
      << info;
}

TEST(InfoReplication, ConnectedReplica) {
  ReplicationStats r = MasterRepl();
  r.replica = true;
  r.master_host = "m1";
  r.master_port = 6379;
  r.link_up = true;
  r.last_io_seconds_ago = 8;
  r.replica_priority = 100;
  EXPECT_EQ(Repl(r).substr(0, Repl(r).find("connected_slaves")),
            "# Replication\r\nrole:slave\r\nmaster_host:m1\r\nmaster_port:6379\r\n"
            "master_link_status:up\r\nmaster_last_io_seconds_ago:8\r\nmaster_sync_in_progress:0\r\n"
            "slave_read_repl_offset:15\r\nslave_repl_offset:15\r\nslave_priority:100\r\n"
            "slave_read_only:1\r\nreplica_announced:1\r\n");
  EXPECT_EQ(Repl(r).find("master_link_down_since_seconds"), std::string::npos);
}

TEST(InfoReplication, DownReplicaShowsDownSince) {
  ReplicationStats r = MasterRepl();
  r.replica = true;
  r.master_host = "m1";
  r.master_port = 6379;
  r.link_down_since_seconds = -1;  // never connected
  std::string info = Repl(r);
  EXPECT_NE(info.find("master_link_status:down\r\nmaster_last_io_seconds_ago:-1\r\n"
                      "master_sync_in_progress:0\r\nslave_read_repl_offset:15\r\n"
                      "slave_repl_offset:15\r\nmaster_link_down_since_seconds:-1\r\nslave_priority:100\r\n"),
            std::string::npos)
      << info;
  r.link_down_since_seconds = 42;
  r.read_only = false;
  info = Repl(r);
  EXPECT_NE(info.find("master_link_down_since_seconds:42\r\n"), std::string::npos);
  EXPECT_NE(info.find("slave_read_only:0\r\n"), std::string::npos);
}

TEST(InfoReplication, PromotedMasterShowsSecondReplid) {
  ReplicationStats r = MasterRepl();
  r.replid2 = std::string(40, 'b');
  r.second_offset = 11;
  const std::string info = Repl(r);
  EXPECT_NE(info.find("master_replid2:" + std::string(40, 'b') + "\r\n"), std::string::npos);
  EXPECT_NE(info.find("second_repl_offset:11\r\n"), std::string::npos);
}

TEST(InfoReplication, KeyspaceHasSequenceInPlainInfoAndSection) {
  ServerStats s = SampleStats();
  s.repl = MasterRepl();
  s.repl.applied_seq = 9;
  EXPECT_NE(FormatInfo(s, {}).find("# Keyspace\r\nsequence:9\r\n"), std::string::npos);
  EXPECT_EQ(FormatInfo(s, {"keyspace"}), "# Keyspace\r\nsequence:9\r\n");
  EXPECT_NE(FormatInfo(s, {}).find("# Replication\r\nrole:master"), std::string::npos);
}

TEST(InfoReplication, StatsHasSyncCounters) {
  ServerStats s = SampleStats();
  s.repl.sync_full = 3;
  s.repl.sync_partial_ok = 2;
  s.repl.sync_partial_err = 1;
  const std::string info = FormatInfo(s, {"stats"});
  EXPECT_NE(info.find("sync_full:3\r\nsync_partial_ok:2\r\nsync_partial_err:1\r\n"), std::string::npos);
}

TEST(InfoReplication, ReplicaDuringAFullSyncShowsTheTransferFields) {  // server.c:6026-6062
  ReplicationStats r;
  r.replica = true;
  r.master_host = "10.0.0.1";
  r.master_port = 7000;
  r.link_up = false;
  r.last_io_seconds_ago = -1;
  r.link_down_since_seconds = -1;
  r.sync_in_progress = true;
  r.sync_total_bytes = 0;
  r.sync_read_bytes = 1234;
  r.sync_last_io_seconds_ago = 0;
  r.replid = std::string(40, 'a');
  const std::string out = FormatReplicationSection(r);
  EXPECT_NE(
      out.find("master_link_status:down\r\nmaster_last_io_seconds_ago:-1\r\nmaster_sync_in_progress:1\r\n"
               "slave_read_repl_offset:0\r\nslave_repl_offset:0\r\nmaster_sync_total_bytes:0\r\n"
               "master_sync_read_bytes:1234\r\nmaster_sync_left_bytes:-1234\r\nmaster_sync_perc:0.00\r\n"
               "master_sync_last_io_seconds_ago:0\r\nmaster_link_down_since_seconds:-1\r\n"),
      std::string::npos)
      << out;
}

TEST(InfoReplication, AReplicaReceivingItsSnapshotIsSendBulk) {
  ReplicationStats r;
  r.replid = std::string(40, 'a');
  r.replicas.push_back({"10.0.0.2", 7001, 0, 0, "send_bulk"});
  r.replicas.push_back({"10.0.0.3", 7002, 9, 1, "online"});
  const std::string out = FormatReplicationSection(r);
  EXPECT_NE(out.find("connected_slaves:2\r\nslave0:ip=10.0.0.2,port=7001,state=send_bulk,offset=0,lag=0\r\n"
                     "slave1:ip=10.0.0.3,port=7002,state=online,offset=9,lag=1\r\n"),
            std::string::npos)
      << out;
}

TEST(Hello, RoleFollowsReplication) {
  CommandHarness h;
  EXPECT_NE(h.Run({"HELLO", "3"}).find("role\r\n$6\r\nmaster\r\n"), std::string::npos);
  h.services.replication().BecomeReplica("m", 1);
  EXPECT_NE(h.Run({"HELLO", "3"}).find("role\r\n$7\r\nreplica\r\n"), std::string::npos);
}

}  // namespace
}  // namespace tideskv
