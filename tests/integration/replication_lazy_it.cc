#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "support/it_helpers.h"
#include "support/test_server.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

// repl-backlog-lazy (the default): a master with no replica writes no replication log records; seqs still
// advance, and the first PSYNC (or slot migration) activates the log with a baseline at the current seq.
namespace tideskv::test {
namespace {

using namespace std::chrono_literals;

std::unique_ptr<RespClient> Client(const TestServer& s) {
  auto c = s.Connect();
  EXPECT_NE(c, nullptr);
  return c;
}

std::string Digest(RespClient& c) { return c.Command({"DEBUG", "DIGEST"}).str; }

bool Synced(RespClient& master, RespClient& replica, std::chrono::milliseconds timeout = 20s) {
  return WaitFor(
      [&] {
        return InfoString(replica, "replication", "master_link_status") == "up" &&
               InfoField(replica, "replication", "slave_repl_offset") ==
                   InfoField(master, "replication", "master_repl_offset");
      },
      timeout);
}

void Writes(RespClient& c, const std::string& prefix, int n) {
  for (int i = 0; i < n; ++i) {
    ASSERT_EQ(c.Command({"SET", prefix + std::to_string(i), "v" + std::to_string(i)}).str, "OK");
    ASSERT_EQ(c.Command({"HSET", prefix + "h", "f" + std::to_string(i), "v"}).integer, 1);
  }
}

TEST(ReplicationLazyIT, ConfigGetShowsTheDefault) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = Client(s);
  const RespValue r = c->Command({"CONFIG", "GET", "repl-backlog-lazy"});
  ASSERT_EQ(r.elements.size(), 2u);
  EXPECT_EQ(r.elements[1].str, "yes");
}

TEST(ReplicationLazyIT, TheFirstReplicaActivatesTheLogThenStreamsAndWaits) {
  TestServer master(TIDESKV_SERVER_BIN);
  auto m = Client(master);
  Writes(*m, "before", 50);
  const int64_t offset = InfoField(*m, "replication", "master_repl_offset");
  EXPECT_GE(offset, 100);  // seqs advance without records (the oid lease commits too)
  EXPECT_EQ(InfoField(*m, "replication", "repl_backlog_active"), 0);
  EXPECT_EQ(InfoField(*m, "replication", "repl_backlog_first_byte_offset"), offset + 1);
  EXPECT_EQ(InfoField(*m, "replication", "repl_backlog_histlen"), 0);
  TestServer replica(TIDESKV_SERVER_BIN,
                     {.extra_args = {"--replicaof", "127.0.0.1 " + std::to_string(master.port())}});
  auto r = Client(replica);
  ASSERT_TRUE(Synced(*m, *r)) << replica.Stderr();
  EXPECT_EQ(InfoField(*m, "replication", "repl_backlog_active"), 1);
  EXPECT_EQ(InfoField(*m, "stats", "sync_full"), 1);
  EXPECT_EQ(InfoField(*m, "stats", "sync_partial_ok"), 0);
  Writes(*m, "after", 20);
  EXPECT_EQ(m->Command({"WAIT", "1", "5000"}).integer, 1);
  ASSERT_TRUE(Synced(*m, *r));
  EXPECT_EQ(Digest(*r), Digest(*m));
  EXPECT_EQ(r->Command({"GET", "before49"}).str, "v49");
  EXPECT_EQ(r->Command({"GET", "after19"}).str, "v19");
}

TEST(ReplicationLazyIT, ADormantMasterKeepsItsOffsetAcrossARestart) {
  TestServer master(TIDESKV_SERVER_BIN);
  auto m = Client(master);
  Writes(*m, "k", 30);
  const int64_t offset = InfoField(*m, "replication", "master_repl_offset");
  m.reset();
  ASSERT_EQ(master.Terminate(), 0);
  master.Restart();
  m = Client(master);
  EXPECT_EQ(InfoField(*m, "replication", "master_repl_offset"), offset);
  EXPECT_EQ(InfoField(*m, "replication", "second_repl_offset"), offset + 1);
  EXPECT_EQ(InfoField(*m, "replication", "repl_backlog_active"), 0);
  EXPECT_EQ(master.Stderr().find("predates the replication log"), std::string::npos);
  ASSERT_EQ(m->Command({"SET", "next", "v"}).str, "OK");
  EXPECT_EQ(InfoField(*m, "replication", "master_repl_offset"), offset + 1);
}

TEST(ReplicationLazyIT, TheReplicaReattachesToARestartedMaster) {
  TestServer master(TIDESKV_SERVER_BIN);
  auto m = Client(master);
  Writes(*m, "a", 20);
  TestServer replica(TIDESKV_SERVER_BIN,
                     {.extra_args = {"--replicaof", "127.0.0.1 " + std::to_string(master.port())}});
  auto r = Client(replica);
  ASSERT_TRUE(Synced(*m, *r)) << replica.Stderr();
  m.reset();
  ASSERT_EQ(master.Terminate(), 0);
  master.Restart();
  m = Client(master);
  EXPECT_EQ(InfoField(*m, "replication", "repl_backlog_active"), 1);  // it had a replica: still logging
  Writes(*m, "b", 20);
  ASSERT_TRUE(Synced(*m, *r, 30s)) << replica.Stderr();
  EXPECT_EQ(Digest(*r), Digest(*m));
  EXPECT_EQ(m->Command({"WAIT", "1", "5000"}).integer, 1);
}

TEST(ReplicationLazyIT, LazyNoKeepsTheLogActiveFromTheStart) {
  TestServer master(TIDESKV_SERVER_BIN, {.extra_args = {"--repl-backlog-lazy", "no"}});
  auto m = Client(master);
  Writes(*m, "k", 5);
  EXPECT_EQ(InfoField(*m, "replication", "repl_backlog_active"), 1);
  EXPECT_EQ(InfoField(*m, "replication", "repl_backlog_first_byte_offset"), 1);
  EXPECT_EQ(InfoField(*m, "replication", "repl_backlog_histlen"),
            InfoField(*m, "replication", "master_repl_offset"));
  ASSERT_EQ(DeclareSlotless(*m), "slotless");
  const RespValue r = m->Command({"PSYNC", "?", "1"});  // an empty replica continues a full log
  EXPECT_EQ(r.str.rfind("CONTINUE", 0), 0u) << r.str;
}

TEST(ReplicationLazyIT, ASlotMigrationActivatesTheSourcesLog) {
  TestServer a(TIDESKV_SERVER_BIN, {.extra_args = {"--cluster-enabled", "yes"}});
  TestServer b(TIDESKV_SERVER_BIN, {.extra_args = {"--cluster-enabled", "yes"}});
  const auto line = [](char id, const TestServer& s, const std::string& slots) {
    return std::string(40, id) + " 127.0.0.1 " + std::to_string(s.port()) + " master - " + slots + "\n";
  };
  const std::string nodes = line('a', a, "0-8191") + line('b', b, "8192-16383");
  for (const auto& [s, id] : {std::pair<const TestServer*, char>{&a, 'a'}, {&b, 'b'}}) {
    auto c = Client(*s);
    ASSERT_EQ(c->Command({"CLUSTERX", "SETNODEID", std::string(40, id)}).str, "OK");
    ASSERT_EQ(c->Command({"CLUSTERX", "SETNODES", nodes, "1"}).str, "OK");
  }
  auto ac = Client(a);
  auto bc = Client(b);
  ASSERT_EQ(ac->Command({"SET", "b", "1"}).str, "OK");  // slot 3300
  EXPECT_EQ(InfoField(*ac, "replication", "repl_backlog_active"), 0);
  ASSERT_EQ(ac->Command({"CLUSTERX", "MIGRATE", "3300", std::string(40, 'b')}).str, "OK");
  ASSERT_TRUE(WaitFor(
      [&] {
        return ac->Command({"CLUSTER", "INFO"}).str.find("migrating_state: success") != std::string::npos;
      },
      30s));
  EXPECT_EQ(InfoField(*ac, "replication", "repl_backlog_active"), 1);
  EXPECT_EQ(bc->Command({"GET", "b"}).str, "1");
}

}  // namespace
}  // namespace tideskv::test
