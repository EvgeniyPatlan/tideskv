#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "support/it_helpers.h"
#include "support/temp_dir.h"
#include "support/test_server.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

// Cluster mode against a real server (M7a). Node ids are any 40 bytes (the controller uses [A-Za-z0-9]).
namespace tideskv::test {
namespace {

using namespace std::chrono_literals;

std::string Id(char c) { return std::string(40, c); }

std::vector<std::string> ClusterArgs() { return {"--cluster-enabled", "yes"}; }

std::string MasterLine(char id, const TestServer& s, const std::string& slots) {
  return Id(id) + " 127.0.0.1 " + std::to_string(s.port()) + " master - " + slots + "\n";
}

std::string SlaveLine(char id, const TestServer& s, char master) {
  return Id(id) + " 127.0.0.1 " + std::to_string(s.port()) + " slave " + Id(master) + "\n";
}

// What kvrocks-controller sends to every node: SETNODEID, then SETNODES (kvrocks-cluster §4.1).
void Push(const TestServer& s, char id, const std::string& nodes, int64_t version) {
  auto c = s.Connect();
  ASSERT_EQ(c->Command({"CLUSTERX", "SETNODEID", Id(id)}).str, "OK");
  ASSERT_EQ(c->Command({"CLUSTERX", "SETNODES", nodes, std::to_string(version)}).str, "OK");
}

std::string LinkOf(RespClient& c) {  // ROLE's link state on a replica, "" on a master
  const RespValue r = c.Command({"ROLE"});
  return r.elements.size() == 5 ? r.elements[3].str : "";
}

bool SameSequence(RespClient& a, RespClient& b) {
  return InfoField(a, "keyspace", "sequence") == InfoField(b, "keyspace", "sequence");
}

TEST(ClusterIT, InfoAndHelloReportClusterMode) {
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--cluster-enabled", "yes"}});
  auto c = s.Connect();
  EXPECT_EQ(InfoField(*c, "cluster", "cluster_enabled"), 1);
  EXPECT_EQ(InfoString(*c, "server", "redis_mode"), "cluster");
  const RespValue got = c->Command({"CONFIG", "GET", "cluster-enabled"});
  ASSERT_EQ(got.elements.size(), 2u);
  EXPECT_EQ(got.elements[1].str, "yes");
  TestServer plain(TIDESKV_SERVER_BIN);
  auto p = plain.Connect();
  EXPECT_EQ(InfoField(*p, "cluster", "cluster_enabled"), 0);
  EXPECT_EQ(InfoString(*p, "server", "redis_mode"), "standalone");
}

TEST(ClusterIT, StartupRefusesKeysOutsideDb0) {  // D19
  TempDir dir("tideskv-cluster-db");
  {
    TestServer s(TIDESKV_SERVER_BIN, {.dir = dir.str()});
    auto c = s.Connect();
    ASSERT_EQ(c->Command({"SELECT", "3"}).str, "OK");
    ASSERT_EQ(c->Command({"SET", "k", "v"}).str, "OK");
    c.reset();
    ASSERT_EQ(s.Terminate(), 0);
  }
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = ClusterArgs(), .dir = dir.str(), .wait_ready = false});
  EXPECT_EQ(s.Wait(), 2);
  EXPECT_NE(s.Stderr().find("databases 1-15 hold keys"), std::string::npos) << s.Stderr();
}

TEST(ClusterIT, StartupRefusesReplicaofWithClusterEnabled) {  // D19
  TestServer s(
      TIDESKV_SERVER_BIN,
      {.extra_args = {"--cluster-enabled", "yes", "--replicaof", "127.0.0.1 6379"}, .wait_ready = false});
  EXPECT_EQ(s.Wait(), 2);
  EXPECT_NE(s.Stderr().find("the cluster controller assigns roles"), std::string::npos) << s.Stderr();
}

TEST(ClusterIT, ClusterxIsRefusedOutsideClusterModeAndReplicaofInside) {
  TestServer plain(TIDESKV_SERVER_BIN);
  EXPECT_EQ(plain.Connect()->Command({"CLUSTERX", "VERSION"}).str, "ERR Cluster mode is not enabled");
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = ClusterArgs()});
  EXPECT_EQ(s.Connect()->Command({"REPLICAOF", "127.0.0.1", "1"}).str,
            "ERR REPLICAOF not allowed in cluster mode.");
}

TEST(ClusterIT, TheTopologySurvivesARestartAndAKill9) {  // D20
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = ClusterArgs()});
  Push(s, 'a', MasterLine('a', s, "0-16383"), 3);
  s.Restart();
  auto c = s.Connect();
  EXPECT_EQ(c->Command({"CLUSTERX", "VERSION"}).str, "3");
  EXPECT_EQ(c->Command({"CLUSTERX", "MYID"}).str, Id('a'));
  ASSERT_EQ(c->Command({"CLUSTERX", "SETNODES", MasterLine('a', s, "0-100"), "4"}).str, "OK");
  c.reset();
  s.Kill9();  // +OK came after the checkpoint: the record must survive
  s.Restart();
  EXPECT_EQ(s.Connect()->Command({"CLUSTERX", "VERSION"}).str, "4");
}

TEST(ClusterIT, SetnodesMakesAReplicaAndRemovingItFromTheTopologyStopsReplicating) {  // D10
  TestServer m(TIDESKV_SERVER_BIN, {.extra_args = ClusterArgs()});
  TestServer r(TIDESKV_SERVER_BIN, {.extra_args = ClusterArgs()});
  const std::string nodes = MasterLine('a', m, "0-16383") + SlaveLine('b', r, 'a');
  Push(m, 'a', nodes, 1);
  Push(r, 'b', nodes, 1);
  auto mc = m.Connect();
  auto rc = r.Connect();
  EXPECT_EQ(InfoString(*rc, "replication", "role"), "slave");
  ASSERT_TRUE(WaitFor([&] { return LinkOf(*rc) == "connected"; }, 30s));
  ASSERT_EQ(mc->Command({"SET", "k", "v"}).str, "OK");
  ASSERT_TRUE(WaitFor([&] { return SameSequence(*mc, *rc); }, 10s));
  EXPECT_EQ(rc->Command({"DEBUG", "DIGEST"}).str, mc->Command({"DEBUG", "DIGEST"}).str);
  const std::string alone = MasterLine('a', m, "0-16383");
  ASSERT_EQ(mc->Command({"CLUSTERX", "SETNODES", alone, "2"}).str, "OK");
  ASSERT_EQ(rc->Command({"CLUSTERX", "SETNODES", alone, "2"}).str, "OK");
  EXPECT_EQ(InfoString(*rc, "replication", "role"), "master");  // Kvrocks #2643
}

// Review Focus #3, D19: a cluster replica boots as a replica (no replid rotation) even while its master is
// unreachable, takes no write, and partial-syncs once the master is back.
TEST(ClusterIT, AReplicaBootsAsAReplicaAndPartialSyncs) {
  TestServer m(TIDESKV_SERVER_BIN, {.extra_args = ClusterArgs()});
  TestServer r(TIDESKV_SERVER_BIN, {.extra_args = ClusterArgs()});
  const std::string nodes = MasterLine('a', m, "0-16383") + SlaveLine('b', r, 'a');
  Push(m, 'a', nodes, 1);
  Push(r, 'b', nodes, 1);
  auto mc = m.Connect();
  auto rc = r.Connect();
  ASSERT_TRUE(WaitFor([&] { return LinkOf(*rc) == "connected"; }, 30s));
  for (int i = 0; i < 100; ++i) ASSERT_EQ(mc->Command({"SET", "k" + std::to_string(i), "v"}).str, "OK");
  ASSERT_TRUE(WaitFor([&] { return SameSequence(*mc, *rc); }, 10s));
  const std::string replid = InfoString(*rc, "replication", "master_replid");
  const int64_t full = InfoField(*mc, "stats", "sync_full");
  const int64_t partial = InfoField(*mc, "stats", "sync_partial_ok");
  rc.reset();
  ASSERT_EQ(r.Terminate(), 0);
  m.Pause();
  r.Restart();
  rc = r.Connect();
  EXPECT_EQ(InfoString(*rc, "replication", "role"), "slave");
  EXPECT_EQ(InfoString(*rc, "replication", "master_replid"), replid);
  EXPECT_EQ(rc->Command({"SET", "x", "1"}).type, RespValue::Type::kError);  // MOVED once Task 12 routes
  m.Resume();
  ASSERT_TRUE(WaitFor([&] { return LinkOf(*rc) == "connected"; }, 30s));
  EXPECT_EQ(InfoField(*mc, "stats", "sync_full"), full);
  EXPECT_EQ(InfoField(*mc, "stats", "sync_partial_ok"), partial + 1);
}

}  // namespace
}  // namespace tideskv::test
