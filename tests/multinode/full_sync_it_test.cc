#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "multinode/multinode_helpers.h"
#include "multinode/workload.h"
#include "support/full_sync.h"
#include "support/temp_dir.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

// Full sync end to end (M6b): under load, every type and db, restarts, link failures mid-transfer,
// repointing during a load, concurrent full syncs, and the hard backlog limit during a transfer.
//
// An empty replica of a master whose log starts at seq 1 gets +CONTINUE (D18), not a full sync. Every
// test here forces the full sync (PF1): the replica writes a key as a standalone node first
// (ForceFullSync), or the master's log is trimmed past seq 1 before the replica connects (WaitTrimmed).
// A replica whose load was aborted is empty again, so its retry is a full sync only on a trimmed log.
namespace tideskv::test {
namespace {

using namespace multinode;

void Fill(RespClient& c, int n, int value_size = 10) {
  const std::string v(value_size, 'x');
  for (int i = 0; i < n; ++i) ASSERT_EQ(c.Command({"SET", "fill:" + std::to_string(i), v}).str, "OK");
}

// Shrinks the master's backlog and waits until the trimmer cut it past seq 1: PSYNC ? 1 is then
// answered +FULLRESYNC (D18), on the first attempt and on every retry.
void WaitTrimmed(RespClient& m, const std::string& backlog_size) {
  ASSERT_EQ(m.Command({"CONFIG", "SET", "repl-backlog-size", backlog_size}).str, "OK");
  ASSERT_TRUE(WaitFor([&] { return InfoField(m, "replication", "repl_backlog_first_byte_offset") > 1; }, 10s))
      << InfoString(m, "replication", "repl_backlog_first_byte_offset");
}

bool Loading(RespClient& c) { return InfoField(c, "persistence", "loading") == 1; }

TEST(FullSyncIT, FullSyncUnderConcurrentWrites) {  // Review Focus #2
  const uint32_t seed = Seed();
  SCOPED_TRACE("seed " + std::to_string(seed));
  TestServer m(TIDESKV_SERVER_BIN);
  auto mc = Client(m);
  Fill(*mc, 20000);
  ASSERT_EQ(mc->Command({"CONFIG", "SET", "rdb-key-save-delay", "100"}).str, "OK");  // ~2 s of transfer
  TestServer r(TIDESKV_SERVER_BIN);
  auto rc = Client(r);
  ASSERT_TRUE(ForceFullSync(*rc));
  Workload load(m.port(), seed, 4);
  load.Start();
  Follow(r, m);
  ASSERT_TRUE(WaitFor([&] { return Loading(*rc); }, 10s));
  const uint64_t acked_at_load = load.acked_writes();  // the snapshot is cut: later writes are the tail
  ASSERT_TRUE(WaitConnected(r, 60s));
  EXPECT_GT(load.acked_writes(), acked_at_load) << "no write landed during the transfer";
  std::this_thread::sleep_for(1s);
  load.Stop();
  ExpectHealthyLoad(load);
  ExpectConverged(m, {&r});
  EXPECT_EQ(InfoField(*mc, "stats", "sync_full"), 1);
  // The tail starts at S + 1 on the same link: a gap would drop it and heal by a partial resync.
  EXPECT_EQ(InfoField(*mc, "stats", "sync_partial_ok"), 0);
}

void WriteEveryType(RespClient& mc) {
  for (int db : {0, 3, 15}) {
    ASSERT_EQ(mc.Command({"SELECT", std::to_string(db)}).str, "OK");
    ASSERT_EQ(mc.Command({"SET", "s", "v", "EX", "1000"}).str, "OK");
    ASSERT_EQ(mc.Command({"HSET", "h", "f", "v"}).integer, 1);
    ASSERT_EQ(mc.Command({"RPUSH", "l", "a", "b", "c"}).integer, 3);
    ASSERT_EQ(mc.Command({"SADD", "set", "a", "b"}).integer, 2);
    ASSERT_EQ(mc.Command({"ZADD", "z", "1.5", "m", "-2", "n"}).integer, 2);
  }
  ASSERT_EQ(mc.Command({"SELECT", "0"}).str, "OK");
  ASSERT_EQ(mc.Command({"SET", "big", std::string(8 << 20, 'b')}).str, "OK");  // two chunks' worth
  for (int i = 0; i < 70000; i += 1000) {  // one hash spanning several chunks
    std::vector<std::string> cmd = {"HSET", "wide"};
    for (int j = i; j < i + 1000; ++j) {
      cmd.push_back("f" + std::to_string(j));
      cmd.push_back("v");
    }
    ASSERT_EQ(mc.Command(cmd).integer, 1000);
  }
}

TEST(FullSyncIT, FullSyncCopiesEveryTypeDbAndTtl) {  // Review Focus #3
  TestServer m(TIDESKV_SERVER_BIN);
  auto mc = Client(m);
  const auto write_start = std::chrono::steady_clock::now();
  WriteEveryType(*mc);
  if (HasFatalFailure()) return;
  // G7: "soon" must outlive the sync and the convergence checks, which move the same data the writes
  // did, so the TTL scales with the measured write time (a TSan build or a loaded runner is slower at
  // both) on top of the 10 s that suffices on an idle machine.
  const auto write_time =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - write_start);
  const std::chrono::milliseconds ttl = 10s + 5 * write_time;
  TestServer r(TIDESKV_SERVER_BIN);
  auto rc = Client(r);
  ASSERT_TRUE(ForceFullSync(*rc));
  ASSERT_EQ(mc->Command({"SET", "soon", "v", "PX", std::to_string(ttl.count())}).str, "OK");
  Follow(r, m);
  ASSERT_TRUE(WaitConnected(r, 60s));
  ExpectConverged(m, {&r});
  EXPECT_EQ(InfoField(*mc, "stats", "sync_full"), 1);
  EXPECT_EQ(rc->Command({"EXISTS", "standalone-key"}).integer, 0);
  ASSERT_EQ(rc->Command({"SELECT", "15"}).str, "OK");
  EXPECT_GT(rc->Command({"TTL", "s"}).integer, 0);
  EXPECT_EQ(rc->Command({"ZSCORE", "z", "n"}).str, "-2");
  EXPECT_EQ(rc->Command({"HLEN", "wide"}).integer, 0);  // db 15 has no "wide"
  ASSERT_EQ(rc->Command({"SELECT", "0"}).str, "OK");
  EXPECT_EQ(rc->Command({"HLEN", "wide"}).integer, 70000);
  EXPECT_EQ(rc->Command({"STRLEN", "big"}).integer, 8 << 20);
  // The snapshot carries the expiry index, not only the TTLs: promoted before "soon" expires (the master
  // gone, so no DEL comes), the replica's own reaper removes it.
  ASSERT_EQ(InfoField(*mc, "stats", "expired_keys"), 0)
      << "\"soon\" (PX " << ttl.count() << ") expired before the master stopped";
  ASSERT_GT(rc->Command({"PTTL", "soon"}).integer, 0);  // still to expire on the replica
  ASSERT_EQ(m.Terminate(), 0);
  ASSERT_EQ(rc->Command({"REPLICAOF", "NO", "ONE"}).str, "OK");
  EXPECT_TRUE(WaitFor([&] { return InfoField(*rc, "stats", "expired_keys") >= 1; }, ttl + 20s));
  EXPECT_EQ(rc->Command({"EXISTS", "soon"}).integer, 0);
}

TEST(FullSyncIT, ReplicaRestartAfterFullSyncContinues) {  // Review Focus #1 (right after EndLoad)
  TestServer m(TIDESKV_SERVER_BIN);
  auto mc = Client(m);
  Fill(*mc, 1000);
  TempDir dir("tideskv-fullsync-replica");
  TestServer r(TIDESKV_SERVER_BIN, {.dir = dir.str()});
  {
    auto rc = Client(r);
    ASSERT_TRUE(ForceFullSync(*rc));
  }
  Follow(r, m);
  ASSERT_TRUE(WaitConnected(r));
  ExpectConverged(m, {&r});
  r.Kill9();
  Fill(*mc, 100, 20);
  r.Restart(ReplicaOfArgs(m));
  ASSERT_TRUE(WaitConnected(r));
  ExpectConverged(m, {&r});
  EXPECT_EQ(InfoField(*mc, "stats", "sync_full"), 1);
  EXPECT_GE(InfoField(*mc, "stats", "sync_partial_ok"), 1);
}

TEST(FullSyncIT, MasterSilentDuringTheTransferTimesOutThenRetries) {  // Review Focus #5
  TestServer m(TIDESKV_SERVER_BIN);
  auto mc = Client(m);
  Fill(*mc, 20000);
  ASSERT_EQ(mc->Command({"CONFIG", "SET", "rdb-key-save-delay", "200"}).str, "OK");  // ~4 s of transfer
  TestServer r(TIDESKV_SERVER_BIN, {.extra_args = {"--repl-timeout", "2"}});
  auto rc = Client(r);
  ASSERT_TRUE(ForceFullSync(*rc));
  Follow(r, m);
  ASSERT_TRUE(WaitFor([&] { return Loading(*rc); }, 10s));
  m.Pause();
  ASSERT_TRUE(WaitFor([&] { return !Loading(*rc); }, 10s));
  EXPECT_EQ(rc->Command({"DBSIZE"}).integer, 0);  // the partial load was discarded, never served
  m.Resume();
  ASSERT_EQ(mc->Command({"CONFIG", "SET", "rdb-key-save-delay", "0"}).str, "OK");
  ASSERT_TRUE(WaitConnected(r, 60s));
  ExpectConverged(m, {&r});
}

TEST(FullSyncIT, ReplicaStalledDuringTheTransferIsDroppedThenRetries) {
  TestServer m(TIDESKV_SERVER_BIN, {.extra_args = {"--repl-timeout", "2"}});
  auto mc = Client(m);
  Fill(*mc, 20000, 2000);  // 40 MB: more than the socket buffers hold
  WaitTrimmed(*mc, "100000");
  if (HasFatalFailure()) return;
  TestServer r(TIDESKV_SERVER_BIN);
  auto rc = Client(r);
  ASSERT_EQ(rc->Command({"CONFIG", "SET", "key-load-delay", "100"}).str, "OK");
  Follow(r, m);
  ASSERT_TRUE(WaitFor([&] { return Loading(*rc); }, 10s));
  r.Pause();
  ASSERT_TRUE(WaitFor([&] { return InfoField(*mc, "replication", "connected_slaves") == 0; }, 20s));
  r.Resume();
  ASSERT_EQ(rc->Command({"CONFIG", "SET", "key-load-delay", "0"}).str, "OK");
  ASSERT_TRUE(WaitConnected(r, 60s));
  ExpectConverged(m, {&r});
  EXPECT_GE(InfoField(*mc, "stats", "sync_full"), 2);
}

TEST(FullSyncIT, ReplicaofAnotherMasterDuringTheLoadAbortsIt) {  // D7
  TestServer a(TIDESKV_SERVER_BIN);
  TestServer b(TIDESKV_SERVER_BIN);
  auto ac = Client(a);
  auto bc = Client(b);
  Fill(*ac, 20000);
  ASSERT_EQ(bc->Command({"SET", "only-on-b", "1"}).str, "OK");
  TestServer r(TIDESKV_SERVER_BIN);
  auto rc = Client(r);
  ASSERT_TRUE(ForceFullSync(*rc));
  ASSERT_EQ(rc->Command({"CONFIG", "SET", "key-load-delay", "400"}).str, "OK");
  Follow(r, a);
  ASSERT_TRUE(WaitFor([&] { return Loading(*rc); }, 10s));
  const RespValue no_one = rc->Command({"REPLICAOF", "NO", "ONE"});
  EXPECT_EQ(no_one.str, "LOADING Redis is loading the dataset in memory");
  ASSERT_EQ(rc->Command({"REPLICAOF", "127.0.0.1", std::to_string(b.port())}).str, "OK");
  ASSERT_EQ(rc->Command({"CONFIG", "SET", "key-load-delay", "0"}).str, "OK");
  ASSERT_TRUE(WaitConnected(r, 60s));
  ExpectConverged(b, {&r});
  EXPECT_EQ(rc->Command({"EXISTS", "fill:0"}).integer, 0);
  EXPECT_EQ(rc->Command({"GET", "only-on-b"}).str, "1");
}

TEST(FullSyncIT, TwoReplicasFullSyncAtOnce) {  // Kvrocks TestReplicationWithMultiSlaves
  TestServer m(TIDESKV_SERVER_BIN);
  auto mc = Client(m);
  Fill(*mc, 5000);
  ASSERT_EQ(mc->Command({"CONFIG", "SET", "rdb-key-save-delay", "400"}).str, "OK");  // ~2 s each
  TestServer r1(TIDESKV_SERVER_BIN);
  TestServer r2(TIDESKV_SERVER_BIN);
  auto c1 = Client(r1);
  ASSERT_EQ(c1->Command({"SET", "own", "1"}).str, "OK");  // foreign data: CONTINUE impossible
  auto c2 = Client(r2);
  ASSERT_EQ(c2->Command({"SET", "own", "2"}).str, "OK");
  Follow(r1, m);
  Follow(r2, m);
  ASSERT_TRUE(WaitFor([&] { return Loading(*c1) && Loading(*c2); }, 10s));  // the transfers overlap
  ASSERT_EQ(mc->Command({"CONFIG", "SET", "rdb-key-save-delay", "0"}).str, "OK");
  ASSERT_TRUE(WaitConnected(r1, 60s));
  ASSERT_TRUE(WaitConnected(r2, 60s));
  ExpectConverged(m, {&r1, &r2});
  EXPECT_EQ(InfoField(*mc, "stats", "sync_full"), 2);
}

// R9 under a transfer (R16: the hard size limit overrides the full-sync pin). The transfer is held
// open far longer than the trimmer's 1 s tick, so the trim that follows the writes always lands
// mid-transfer: no race against the trimmer.
TEST(FullSyncIT, TrimDuringATransferDropsAndRetries) {  // Review Focus #2
  TestServer m(TIDESKV_SERVER_BIN);
  auto mc = Client(m);
  Fill(*mc, 20000);
  WaitTrimmed(*mc, "2000");
  if (HasFatalFailure()) return;
  ASSERT_EQ(mc->Command({"CONFIG", "SET", "rdb-key-save-delay", "1000"}).str, "OK");  // ~20 s
  TestServer r(TIDESKV_SERVER_BIN, {.extra_args = ReplicaOfArgs(m)});
  auto rc = Client(r);
  ASSERT_TRUE(WaitFor([&] { return Loading(*rc); }, 10s));
  EXPECT_EQ(InfoField(*mc, "stats", "sync_full"), 1);
  const size_t log_mark = m.Stderr().size();
  Fill(*mc, 300, 100);  // past the 2000-byte backlog while the snapshot streams: the sender is dropped
  const auto dropped = [&] { return m.Stderr().find("the backlog is trimmed through", log_mark); };
  ASSERT_TRUE(WaitFor([&] { return dropped() != std::string::npos; }, 10s)) << m.Stderr().substr(log_mark);
  ASSERT_TRUE(WaitFor([&] { return InfoField(*mc, "stats", "sync_full") >= 2; }, 30s));
  ASSERT_EQ(mc->Command({"CONFIG", "SET", "rdb-key-save-delay", "0"}).str, "OK");
  ASSERT_TRUE(WaitConnected(r, 60s));
  ExpectConverged(m, {&r});
}

}  // namespace
}  // namespace tideskv::test
