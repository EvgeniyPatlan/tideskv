#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "multinode/multinode_helpers.h"
#include "multinode/workload.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

// L5 (M6a exit criterion): promotion, demotion and repointing follow the PSYNC rule (D8) through the
// replid history (D7): a node at or behind the new master continues, one ahead of it gets FULLRESYNC
// (implemented in M6b, so it stays down) and is never merged.
namespace tideskv::test {
namespace {

using namespace multinode;

constexpr int kClients = 4;
constexpr const char* kFullResyncLog = "is required; it is implemented in M6b";

// A FULLRESYNC leaves an M6a replica down: it logs the M6b line, never connects, applies nothing.
void ExpectStaysDown(TestServer& replica, size_t log_mark, int64_t seq) {
  EXPECT_TRUE(
      WaitFor([&] { return replica.Stderr().find(kFullResyncLog, log_mark) != std::string::npos; }, 10s))
      << replica.Stderr().substr(log_mark);
  auto c = Client(replica);
  bool connected = false;
  const auto until = std::chrono::steady_clock::now() + 1500ms;
  while (!connected && std::chrono::steady_clock::now() < until) {
    connected = LinkState(*c) == "connected";
    std::this_thread::sleep_for(50ms);
  }
  EXPECT_FALSE(connected) << "the replica's link came up after a FULLRESYNC";
  EXPECT_EQ(Sequence(*c), seq);
}

TEST(Failover, PromoteReplicaThenDemoteOldMasterPartialSyncs) {
  const uint32_t seed = Seed();
  SCOPED_TRACE("seed " + std::to_string(seed));
  TestServer a(TIDESKV_SERVER_BIN);
  TestServer b(TIDESKV_SERVER_BIN, {.extra_args = ReplicaOfArgs(a)});
  ASSERT_TRUE(WaitConnected(b));
  {
    // No TTLs: the old master's reaper must not write between the promotion and its demotion, or its
    // seq would pass the new master's history end_seq and FULLRESYNC would be the correct answer.
    Workload load(a.port(), seed, kClients, Workload::Mix::kNoExpiry);
    load.Start();
    std::this_thread::sleep_for(3s);
    load.Stop();
    ExpectHealthyLoad(load);
  }
  ExpectConverged(a, {&b});
  auto ac = Client(a);
  auto bc = Client(b);
  ASSERT_EQ(bc->Command({"REPLICAOF", "NO", "ONE"}).str, "OK");
  const int64_t partial_before = InfoField(*bc, "stats", "sync_partial_ok");
  ASSERT_EQ(ac->Command({"REPLICAOF", "127.0.0.1", std::to_string(b.port())}).str, "OK");
  ASSERT_TRUE(WaitConnected(a)) << a.Stderr();
  EXPECT_TRUE(WaitFor([&] { return InfoField(*bc, "stats", "sync_partial_ok") > partial_before; }));
  EXPECT_EQ(InfoField(*bc, "stats", "sync_full"), 0);
  {
    Workload load(b.port(), seed + 1, kClients);
    load.Start();
    std::this_thread::sleep_for(3s);
    load.Stop();
    ExpectHealthyLoad(load);
  }
  ExpectConverged(b, {&a});
}

// The master dies under load; replica A is promoted and B is pointed at it. D8 via A's history decides:
// B continues iff it is at or behind A (and within A's backlog), else FULLRESYNC.
TEST(Failover, PromoteUnderLoadThenRepointSecondReplica) {
  const uint32_t seed = Seed();
  SCOPED_TRACE("seed " + std::to_string(seed));
  TestServer m(TIDESKV_SERVER_BIN);
  TestServer a(TIDESKV_SERVER_BIN, {.extra_args = ReplicaOfArgs(m)});
  TestServer b(TIDESKV_SERVER_BIN, {.extra_args = ReplicaOfArgs(m)});
  ASSERT_TRUE(WaitConnected(a));
  ASSERT_TRUE(WaitConnected(b));
  Workload load(m.port(), seed, kClients);
  load.Start();
  std::this_thread::sleep_for(3s);
  m.Kill9();
  load.Stop();
  ReportLoad(load);
  auto ac = Client(a);
  auto bc = Client(b);
  // Once a link is down its buffered frames are applied; replicas do not expire keys, so seqs are frozen.
  ASSERT_TRUE(WaitFor([&] { return LinkState(*ac) != "connected" && LinkState(*bc) != "connected"; }, 30s));
  const int64_t a_seq = Sequence(*ac);
  const int64_t b_seq = Sequence(*bc);
  const int64_t a_first = InfoField(*ac, "replication", "repl_backlog_first_byte_offset");
  ASSERT_GT(b_seq, 0);
  const bool expect_continue = b_seq <= a_seq && b_seq + 1 >= a_first;
  std::printf("[ multinode ] A seq %lld, B seq %lld, A backlog from %lld: expect %s\n",
              static_cast<long long>(a_seq), static_cast<long long>(b_seq), static_cast<long long>(a_first),
              expect_continue ? "CONTINUE" : "FULLRESYNC");
  ASSERT_EQ(ac->Command({"REPLICAOF", "NO", "ONE"}).str, "OK");
  const size_t log_mark = b.Stderr().size();
  ASSERT_EQ(bc->Command({"REPLICAOF", "127.0.0.1", std::to_string(a.port())}).str, "OK");
  if (expect_continue) {
    ASSERT_TRUE(WaitConnected(b)) << b.Stderr().substr(log_mark);
    EXPECT_EQ(InfoField(*ac, "stats", "sync_partial_ok"), 1);
    EXPECT_EQ(InfoField(*ac, "stats", "sync_full"), 0);
    ExpectConverged(a, {&b});
  } else {
    ExpectStaysDown(b, log_mark, b_seq);
    EXPECT_GE(InfoField(*ac, "stats", "sync_full"), 1);
    EXPECT_EQ(InfoField(*ac, "stats", "sync_partial_ok"), 0);
  }
}

// F7: the case above with B strictly behind A, made certain. Pausing B alone is not enough (frames
// already in its socket buffers would still arrive once it thaws), so the master drops B's silent link
// (repl-timeout 3 s while B is paused; above the 1 s ACK interval so A stays linked) and the load runs on for
// 200 ms that A applies and B never receives. Then the master dies: A's backlog serves B, which continues and
// converges.
TEST(Failover, PromoteThenRepointASecondReplicaThatIsBehind) {
  const uint32_t seed = Seed();
  SCOPED_TRACE("seed " + std::to_string(seed));
  TestServer m(TIDESKV_SERVER_BIN);
  TestServer a(TIDESKV_SERVER_BIN, {.extra_args = ReplicaOfArgs(m)});
  TestServer b(TIDESKV_SERVER_BIN, {.extra_args = ReplicaOfArgs(m)});
  ASSERT_TRUE(WaitConnected(a));
  ASSERT_TRUE(WaitConnected(b));
  auto mc = Client(m);
  Workload load(m.port(), seed, kClients);
  load.Start();
  std::this_thread::sleep_for(1s);
  b.Pause();
  ASSERT_EQ(mc->Command({"CONFIG", "SET", "repl-timeout", "3"}).str, "OK");
  ASSERT_TRUE(WaitFor([&] { return InfoField(*mc, "replication", "connected_slaves") == 1; }, 30s));
  ASSERT_EQ(mc->Command({"CONFIG", "SET", "repl-timeout", "60"}).str, "OK");
  const uint64_t acked_at_drop = load.acked_writes();
  std::this_thread::sleep_for(200ms);  // writes B will never get from this master
  ASSERT_GT(load.acked_writes(), acked_at_drop);
  m.Kill9();
  load.Stop();
  ReportLoad(load);
  b.Resume();
  auto ac = Client(a);
  auto bc = Client(b);
  ASSERT_TRUE(WaitFor([&] { return LinkState(*ac) != "connected" && LinkState(*bc) != "connected"; }, 30s));
  const int64_t a_seq = Sequence(*ac);
  const int64_t b_seq = Sequence(*bc);
  const int64_t a_first = InfoField(*ac, "replication", "repl_backlog_first_byte_offset");
  std::printf("[ multinode ] A seq %lld, B seq %lld, A backlog from %lld\n", static_cast<long long>(a_seq),
              static_cast<long long>(b_seq), static_cast<long long>(a_first));
  ASSERT_GT(b_seq, 0);
  ASSERT_LT(b_seq, a_seq);
  ASSERT_GE(b_seq + 1, a_first);
  ASSERT_EQ(ac->Command({"REPLICAOF", "NO", "ONE"}).str, "OK");
  const size_t log_mark = b.Stderr().size();
  ASSERT_EQ(bc->Command({"REPLICAOF", "127.0.0.1", std::to_string(a.port())}).str, "OK");
  ASSERT_TRUE(WaitConnected(b)) << b.Stderr().substr(log_mark);
  EXPECT_EQ(InfoField(*ac, "stats", "sync_partial_ok"), 1);
  EXPECT_EQ(InfoField(*ac, "stats", "sync_full"), 0);
  ExpectConverged(a, {&b});
}

// Review Focus #3: writes the old master took while its replica was cut off are never merged into the
// promoted replica; the old master, ahead of the new master's history, gets FULLRESYNC and stays down.
TEST(Failover, OldMasterWithUnreplicatedWritesNeverMerges) {
  TestServer m(TIDESKV_SERVER_BIN);
  TestServer r(TIDESKV_SERVER_BIN, {.extra_args = ReplicaOfArgs(m)});
  ASSERT_TRUE(WaitConnected(r));
  auto mc = Client(m);
  auto rc = Client(r);
  for (int i = 0; i < 100; ++i) ASSERT_EQ(mc->Command({"SET", "base:" + std::to_string(i), "v"}).str, "OK");
  ExpectConverged(m, {&r});
  // Freeze the replica, then let the master drop its silent link: nothing written after that is sent.
  r.Pause();
  ASSERT_EQ(mc->Command({"CONFIG", "SET", "repl-timeout", "1"}).str, "OK");
  ASSERT_TRUE(WaitFor([&] { return InfoField(*mc, "replication", "connected_slaves") == 0; }, 10s));
  std::vector<std::string> exists_lost = {"EXISTS"};
  for (int i = 0; i < 5; ++i) {
    const std::string key = "lost:" + std::to_string(i);
    ASSERT_EQ(mc->Command({"SET", key, "v"}).str, "OK");
    exists_lost.push_back(key);
  }
  const std::string m_digest = Digest(*mc);
  const int64_t m_seq = Sequence(*mc);
  // The thawed replica must not reconnect before it is promoted: with a password on the master (this
  // connection stays authenticated) its PSYNC is refused with NOAUTH and it backs off.
  ASSERT_EQ(mc->Command({"CONFIG", "SET", "requirepass", "f7-no-reconnect"}).str, "OK");
  r.Resume();
  ASSERT_EQ(rc->Command({"REPLICAOF", "NO", "ONE"}).str, "OK");
  ASSERT_EQ(mc->Command({"CONFIG", "SET", "requirepass", ""}).str, "OK");
  EXPECT_EQ(rc->Command(exists_lost).integer, 0);
  ASSERT_LT(Sequence(*rc), m_seq);
  const size_t log_mark = m.Stderr().size();
  ASSERT_EQ(mc->Command({"REPLICAOF", "127.0.0.1", std::to_string(r.port())}).str, "OK");
  ExpectStaysDown(m, log_mark, m_seq);
  EXPECT_GE(InfoField(*rc, "stats", "sync_full"), 1);
  EXPECT_EQ(Digest(*mc), m_digest);  // the old master's keys are unchanged
  EXPECT_EQ(mc->Command(exists_lost).integer, 5);
  EXPECT_EQ(rc->Command(exists_lost).integer, 0);  // and the new master never got them
}

}  // namespace
}  // namespace tideskv::test
