#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "support/it_helpers.h"
#include "support/temp_dir.h"
#include "support/test_server.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

// Two real servers (M6a Task 12): a replica follows a master through REPLICAOF, serves reads, refuses
// writes, survives a restart with a partial resync, and is promoted with REPLICAOF NO ONE.
namespace tideskv::test {
namespace {

using Type = RespValue::Type;
using namespace std::chrono_literals;

std::unique_ptr<RespClient> Client(const TestServer& s) {
  auto c = s.Connect();
  EXPECT_NE(c, nullptr);
  return c;
}

std::string Digest(RespClient& c) { return c.Command({"DEBUG", "DIGEST"}).str; }

// ROLE's offset: the master's last seq, or the replica's applied seq (-1 while its link is down).
int64_t RoleOffset(RespClient& c) {
  const RespValue r = c.Command({"ROLE"});
  if (r.type != Type::kArray || r.elements.empty()) return -2;
  if (r.elements[0].str == "master") return r.elements[1].integer;
  return r.elements.size() == 5 ? r.elements[4].integer : -2;
}

std::string LinkState(RespClient& c) {
  const RespValue r = c.Command({"ROLE"});
  return r.type == Type::kArray && r.elements.size() == 5 ? r.elements[3].str : "";
}

// Waits until the replica's link is up and it has applied everything the master has.
bool Synced(RespClient& master, RespClient& replica, std::chrono::milliseconds timeout = 10s) {
  return WaitFor(
      [&] {
        const int64_t m = RoleOffset(master);
        return LinkState(replica) == "connected" && RoleOffset(replica) == m;
      },
      timeout);
}

// 1000 writes over every type, with deletes, expiries and counters mixed in.
void MixedWrites(RespClient& c, int n) {
  for (int i = 0; i < n; ++i) {
    const std::string s = std::to_string(i);
    RespValue r;
    switch (i % 8) {
      case 0:
        r = c.Command({"SET", "k" + s, "v" + s});
        break;
      case 1:
        r = c.Command({"HSET", "h" + std::to_string(i % 50), "f" + s, s});
        break;
      case 2:
        r = c.Command({"LPUSH", "l" + std::to_string(i % 20), s});
        break;
      case 3:
        r = c.Command({"ZADD", "z" + std::to_string(i % 30), s, "m" + s});
        break;
      case 4:
        r = c.Command({"SADD", "s" + std::to_string(i % 40), "m" + s});
        break;
      case 5:
        r = c.Command({"DEL", "k" + std::to_string(i - 5)});
        break;
      case 6:
        r = c.Command({"INCRBY", "counter", s});
        break;
      default:
        r = c.Command({"EXPIRE", "h" + std::to_string(i % 50), "100000"});
        break;
    }
    ASSERT_NE(r.type, Type::kError) << i << ": " << r.str;
  }
}

struct ReplicationIT : ::testing::Test {
  TestServer master{TIDESKV_SERVER_BIN};
  std::unique_ptr<TestServer> replica = std::make_unique<TestServer>(TIDESKV_SERVER_BIN);

  void Follow(TestServer& r) {
    auto c = Client(r);
    EXPECT_EQ(c->Command({"REPLICAOF", "127.0.0.1", std::to_string(master.port())}).str, "OK");
  }
};

TEST_F(ReplicationIT, ReplicaFollowsMaster) {
  Follow(*replica);
  auto m = Client(master);
  auto r = Client(*replica);
  MixedWrites(*m, 1000);
  ASSERT_TRUE(Synced(*m, *r)) << "master " << RoleOffset(*m) << ", replica " << RoleOffset(*r);
  EXPECT_EQ(Digest(*r), Digest(*m));
  EXPECT_NE(Digest(*m), std::string(40, '0'));
  EXPECT_EQ(r->Command({"HGET", "h1", "f1"}).str, "1");  // the replica serves reads
}

// INFO's offsets are seqs: the master's master_repl_offset equals the replica's slave_repl_offset, and the
// master's slave0 offset (the replica's ACK) converges to it within the ACK cadence.
TEST_F(ReplicationIT, InfoReplicationOffsetsAgreeAfterSync) {
  Follow(*replica);
  auto m = Client(master);
  auto r = Client(*replica);
  MixedWrites(*m, 200);
  ASSERT_TRUE(Synced(*m, *r));
  const int64_t offset = InfoField(*m, "replication", "master_repl_offset");
  EXPECT_GT(offset, 0);
  EXPECT_EQ(InfoField(*r, "replication", "slave_repl_offset"), offset);
  EXPECT_EQ(InfoString(*r, "replication", "role"), "slave");
  EXPECT_EQ(InfoString(*r, "replication", "master_link_status"), "up");
  EXPECT_EQ(InfoField(*r, "keyspace", "sequence"), offset);
  EXPECT_EQ(InfoField(*m, "keyspace", "sequence"), offset);
  EXPECT_EQ(InfoField(*m, "stats", "sync_full") + InfoField(*m, "stats", "sync_partial_ok"), 1);
  const auto acked = [&] {
    const std::string line = InfoString(*m, "replication", "slave0");
    return line.find(",offset=" + std::to_string(offset) + ",") != std::string::npos;
  };
  EXPECT_TRUE(WaitFor(acked, 2s)) << InfoString(*m, "replication", "slave0");
}

TEST_F(ReplicationIT, ReplicaIsReadOnly) {
  Follow(*replica);
  auto r = Client(*replica);
  const RespValue set = r->Command({"SET", "x", "1"});
  EXPECT_EQ(set.type, Type::kError);
  EXPECT_EQ(set.str, "READONLY You can't write against a read only replica.");
  EXPECT_EQ(r->Command({"GET", "x"}).type, Type::kNull);
}

// D13: expired keys are hidden on the replica but stay stored until the master's DEL arrives.
TEST_F(ReplicationIT, ReplicaHidesExpiredKeysButKeepsThemUntilMasterDeletes) {
  Follow(*replica);
  auto m = Client(master);
  auto r = Client(*replica);
  // The master must be paused while `e` is still live there, or its reaper's DEL could reach the replica
  // first. A sync slower than the margin (a loaded machine) retries with a fresh key once the old one
  // is gone from both nodes.
  constexpr auto kTtl = 1000ms;
  constexpr auto kMargin = 600ms;  // pause no later than this after the SET
  bool paused = false;
  for (int attempt = 0; attempt < 5 && !paused; ++attempt) {
    ASSERT_TRUE(WaitFor([&] { return r->Command({"DBSIZE"}).integer == 0; }, 10s));
    const auto set_at = std::chrono::steady_clock::now();
    ASSERT_EQ(m->Command({"SET", "e", "v", "PX", std::to_string(kTtl.count())}).str, "OK");
    ASSERT_TRUE(Synced(*m, *r));
    if (std::chrono::steady_clock::now() - set_at > kMargin) continue;
    master.Pause();  // freezes the master's reaper: no DEL can be replicated
    paused = true;
    std::this_thread::sleep_until(set_at + kTtl + 300ms);
  }
  ASSERT_TRUE(paused) << "the replica never synced within " << kMargin.count() << " ms";
  EXPECT_EQ(r->Command({"GET", "e"}).type, Type::kNull);
  EXPECT_EQ(r->Command({"EXISTS", "e"}).integer, 0);
  EXPECT_EQ(r->Command({"DBSIZE"}).integer, 1);  // still stored: the replica's reaper does not run
  master.Resume();
  EXPECT_TRUE(WaitFor([&] { return r->Command({"DBSIZE"}).integer == 0; }, 10s));
}

TEST_F(ReplicationIT, PromotionKeepsDataAndAcceptsWrites) {
  Follow(*replica);
  auto m = Client(master);
  auto r = Client(*replica);
  MixedWrites(*m, 200);
  ASSERT_TRUE(Synced(*m, *r));
  const std::string digest = Digest(*m);
  EXPECT_EQ(r->Command({"REPLICAOF", "NO", "ONE"}).str, "OK");
  const RespValue role = r->Command({"ROLE"});
  ASSERT_EQ(role.type, Type::kArray);
  EXPECT_EQ(role.elements[0].str, "master");
  EXPECT_EQ(Digest(*r), digest);
  EXPECT_EQ(r->Command({"SET", "after", "promotion"}).str, "OK");
  EXPECT_EQ(r->Command({"HSET", "newhash", "f", "v"}).integer, 1);  // a fresh oid lease above the master's
  EXPECT_EQ(r->Command({"GET", "after"}).str, "promotion");
}

TEST_F(ReplicationIT, ReplicaRestartResumesWithPartialSync) {
  TempDir dir("tideskv-it-replica");
  TestServer::Options o;
  o.dir = dir.str();
  replica = std::make_unique<TestServer>(TIDESKV_SERVER_BIN, o);
  Follow(*replica);
  auto m = Client(master);
  {
    auto r = Client(*replica);
    MixedWrites(*m, 200);
    ASSERT_TRUE(Synced(*m, *r));
  }
  ASSERT_EQ(replica->Terminate(), 0);
  const int64_t applied = RoleOffset(*m);
  MixedWrites(*m, 100);
  // CF11: the flag takes "host port" as one argument.
  o.extra_args = {"--replicaof", "127.0.0.1 " + std::to_string(master.port())};
  replica = std::make_unique<TestServer>(TIDESKV_SERVER_BIN, o);
  auto r = Client(*replica);
  ASSERT_TRUE(Synced(*m, *r));
  EXPECT_EQ(Digest(*r), Digest(*m));
  // A partial resync: the master streamed from where the replica stopped, not from seq 1. (INFO's
  // sync_partial_ok counter arrives with Task 13.)
  EXPECT_NE(master.Stderr().find("streaming from seq " + std::to_string(applied + 1)), std::string::npos)
      << master.Stderr();
}

TEST_F(ReplicationIT, ReplicaWithForeignDataGetsFullResyncAndStaysDown) {
  auto r = Client(*replica);
  for (int i = 0; i < 3; ++i) ASSERT_EQ(r->Command({"SET", "own" + std::to_string(i), "v"}).str, "OK");
  const std::string digest = Digest(*r);
  auto m = Client(master);
  ASSERT_EQ(m->Command({"SET", "theirs", "v"}).str, "OK");
  Follow(*replica);
  EXPECT_TRUE(WaitFor(
      [&] { return replica->Stderr().find("is required; it is implemented in M6b") != std::string::npos; },
      10s))
      << replica->Stderr();
  const auto until = std::chrono::steady_clock::now() + 1500ms;
  while (std::chrono::steady_clock::now() < until) {
    EXPECT_NE(LinkState(*r), "connected");
    std::this_thread::sleep_for(50ms);
  }
  EXPECT_EQ(RoleOffset(*r), -1);
  EXPECT_EQ(Digest(*r), digest);
  EXPECT_EQ(r->Command({"EXISTS", "theirs"}).integer, 0);
}

// D18: a master whose backlog was trimmed past what a stopped replica holds answers its PSYNC with
// +FULLRESYNC; M6a has no full sync, so the replica stays down, logs the M6b line and applies nothing.
TEST_F(ReplicationIT, TrimmedBacklogForcesFullResyncForAFarBehindReplica) {
  TempDir dir("tideskv-it-replica");
  TestServer::Options o;
  o.dir = dir.str();
  replica = std::make_unique<TestServer>(TIDESKV_SERVER_BIN, o);
  Follow(*replica);
  auto m = Client(master);
  {
    auto r = Client(*replica);
    MixedWrites(*m, 100);
    ASSERT_TRUE(Synced(*m, *r));
  }
  ASSERT_EQ(replica->Terminate(), 0);
  const int64_t applied = RoleOffset(*m);
  ASSERT_EQ(m->Command({"CONFIG", "SET", "repl-backlog-size", "2000"}).str, "OK");
  const int64_t full_before = InfoField(*m, "stats", "sync_full");
  MixedWrites(*m, 200);
  // The trimmer ticks every second; it keeps about 2000 bytes, far less than the 200 batches.
  ASSERT_TRUE(WaitFor(
      [&] { return InfoField(*m, "replication", "repl_backlog_first_byte_offset") > applied + 1; }, 10s))
      << InfoString(*m, "replication", "repl_backlog_first_byte_offset");
  EXPECT_LT(InfoField(*m, "replication", "repl_backlog_histlen"), 200);
  EXPECT_EQ(InfoField(*m, "replication", "repl_backlog_first_byte_offset") +
                InfoField(*m, "replication", "repl_backlog_histlen") - 1,
            RoleOffset(*m));
  o.extra_args = {"--replicaof", "127.0.0.1 " + std::to_string(master.port())};
  replica = std::make_unique<TestServer>(TIDESKV_SERVER_BIN, o);
  auto r = Client(*replica);
  EXPECT_TRUE(WaitFor(
      [&] { return replica->Stderr().find("is required; it is implemented in M6b") != std::string::npos; },
      10s))
      << replica->Stderr();
  EXPECT_TRUE(WaitFor([&] { return InfoField(*m, "stats", "sync_full") > full_before; }, 5s));
  EXPECT_NE(LinkState(*r), "connected");
  EXPECT_EQ(InfoField(*r, "keyspace", "sequence"), applied);  // nothing was applied
}

// R12 (F1): a master takes a fresh replid at every start; the old one stays valid up to its last seq.
TEST_F(ReplicationIT, MasterRestartRotatesTheReplid) {
  auto m = Client(master);
  MixedWrites(*m, 50);
  const std::string old_id = InfoString(*m, "replication", "master_replid");
  const int64_t last = InfoField(*m, "replication", "master_repl_offset");
  ASSERT_GT(last, 0);
  m.reset();
  ASSERT_EQ(master.Terminate(), 0);
  master.Restart();
  m = Client(master);
  const std::string new_id = InfoString(*m, "replication", "master_replid");
  EXPECT_EQ(new_id.size(), 40u);
  EXPECT_NE(new_id, old_id);
  EXPECT_EQ(InfoString(*m, "replication", "master_replid2"), old_id);
  EXPECT_EQ(InfoField(*m, "replication", "second_repl_offset"), last + 1);
}

// R12 (F1): a replica at the restarted master's last seq continues through the history entry.
TEST_F(ReplicationIT, ReplicaContinuesAfterTheMasterRestarts) {
  Follow(*replica);
  {
    auto m = Client(master);
    auto r = Client(*replica);
    MixedWrites(*m, 100);
    ASSERT_TRUE(Synced(*m, *r));
  }
  ASSERT_EQ(master.Terminate(), 0);
  master.Restart();
  auto m = Client(master);
  auto r = Client(*replica);
  MixedWrites(*m, 100);
  ASSERT_TRUE(Synced(*m, *r, 30s)) << replica->Stderr();
  EXPECT_GE(InfoField(*m, "stats", "sync_partial_ok"), 1);
  EXPECT_EQ(InfoField(*m, "stats", "sync_full"), 0);
  EXPECT_EQ(Digest(*r), Digest(*m));
  EXPECT_EQ(InfoString(*r, "replication", "master_replid"), InfoString(*m, "replication", "master_replid"));
}

// R12 (F1): a master that lost a tail its replica had applied (an OS crash under sync-mode interval;
// simulated by restoring an older copy of its data dir) re-issues those seqs with other writes. Its
// replid rotated when it restarted, so the replica's id is unknown to it: FULLRESYNC, never CONTINUE.
TEST_F(ReplicationIT, ReplicaAheadOfARestartedMasterGetsFullResync) {
  namespace fs = std::filesystem;
  TempDir replica_dir("tideskv-it-replica");
  TestServer::Options o;
  o.dir = replica_dir.str();
  replica = std::make_unique<TestServer>(TIDESKV_SERVER_BIN, o);
  Follow(*replica);
  auto m = Client(master);
  auto r = Client(*replica);
  MixedWrites(*m, 50);
  ASSERT_TRUE(Synced(*m, *r));
  m.reset();
  ASSERT_EQ(master.Terminate(), 0);
  TempDir backup("tideskv-it-backup");
  fs::copy(master.dir(), backup.path(), fs::copy_options::recursive);  // the master as of now
  master.Restart();
  m = Client(master);
  MixedWrites(*m, 50);  // seqs past the backup, which the replica applies
  ASSERT_TRUE(Synced(*m, *r, 30s)) << replica->Stderr();
  const int64_t replica_seq = RoleOffset(*r);
  r.reset();
  ASSERT_EQ(replica->Terminate(), 0);
  m.reset();
  ASSERT_EQ(master.Terminate(), 0);
  fs::remove_all(master.dir());  // the tail is lost
  fs::copy(backup.path(), master.dir(), fs::copy_options::recursive);
  master.Restart();
  m = Client(master);
  for (int i = 0; InfoField(*m, "keyspace", "sequence") <= replica_seq + 5; ++i) {
    ASSERT_EQ(m->Command({"SET", "other" + std::to_string(i), "v"}).str, "OK");
  }
  o.extra_args = {"--replicaof", "127.0.0.1 " + std::to_string(master.port())};
  replica = std::make_unique<TestServer>(TIDESKV_SERVER_BIN, o);
  EXPECT_TRUE(WaitFor(
      [&] { return replica->Stderr().find("is required; it is implemented in M6b") != std::string::npos; },
      10s))
      << replica->Stderr();
  EXPECT_GE(InfoField(*m, "stats", "sync_full"), 1);
  EXPECT_EQ(InfoField(*m, "stats", "sync_partial_ok"), 0);
  r = Client(*replica);
  EXPECT_EQ(InfoField(*r, "keyspace", "sequence"), replica_seq);  // nothing was applied
}

// F3: a replica whose applier waits for the gate behind a busy no-writes script still shuts down
// promptly: the script is killed before the replication link is joined.
TEST_F(ReplicationIT, ReplicaShutdownIsNotHeldUpByABusyScript) {
  Follow(*replica);
  auto m = Client(master);
  auto r = Client(*replica);
  ASSERT_EQ(m->Command({"SET", "before", "v"}).str, "OK");
  ASSERT_TRUE(Synced(*m, *r));
  auto busy = Client(*replica);
  ASSERT_TRUE(busy->Send({"EVAL", "local i = 0 while true do i = i + 1 end", "0"}));
  std::this_thread::sleep_for(200ms);  // the script holds the gate
  ASSERT_EQ(m->Command({"SET", "during", "v"}).str, "OK");
  std::this_thread::sleep_for(300ms);  // the applier now waits for the gate
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(replica->Terminate(), 0) << replica->Stderr();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
}

}  // namespace
}  // namespace tideskv::test
