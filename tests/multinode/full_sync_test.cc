#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "multinode/multinode_helpers.h"
#include "multinode/workload.h"
#include "support/full_sync.h"
#include "support/temp_dir.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

// The M6b exit criterion (spec §12 M6): kill -9 of either node during a full sync, then convergence.
// Review Focus #1: a replica killed at any point of a load (between the start txn and the first chunk,
// between chunks, after the end txn but before the master saw an ACK) restarts into an empty keyspace or
// the complete one, never a partial one served as data, and then converges.
//
// Every first sync here is forced (PF1): the replica writes a key as a standalone node (ForceFullSync), or
// the master's log is trimmed past seq 1. A replica whose load was discarded retries with PSYNC ? 1, which
// an untrimmed log answers +CONTINUE (CF-M6b-9): an empty keyspace plus the whole log converges as well.
namespace tideskv::test {
namespace {

using namespace multinode;

constexpr int kKeys = 20000;
constexpr int kValueBytes = 1000;  // 20 MB of values: about five 4 MiB LOAD chunks
constexpr int kKeysPerMset = 32;   // under TSan's 64 locks held at once: one key stripe each
const std::string kZeroDigest(40, '0');

// `n` keys "fill:<i>" of `value_bytes` each, kKeysPerMset per MSET.
void Fill(RespClient& c, int n, int value_bytes) {
  const std::string v(value_bytes, 'x');
  for (int i = 0; i < n; i += kKeysPerMset) {
    std::vector<std::string> cmd = {"MSET"};
    for (int j = i; j < std::min(n, i + kKeysPerMset); ++j) {
      cmd.push_back("fill:" + std::to_string(j));
      cmd.push_back(v);
    }
    ASSERT_EQ(c.Command(cmd).str, "OK");
  }
}

bool Loading(RespClient& c) { return InfoField(c, "persistence", "loading") == 1; }

// The marker is committed: loading() is set at the barrier, loading_start_time only once Begin returned.
bool LoadStarted(RespClient& c) {
  return Loading(c) && InfoField(c, "persistence", "loading_start_time") > 0;
}

void ExpectNoSanitizerReport(const TestServer& s) {
  const std::string log = s.Stderr();
  EXPECT_EQ(log.find("WARNING: ThreadSanitizer"), std::string::npos) << log;
  EXPECT_EQ(log.find("ERROR: AddressSanitizer"), std::string::npos) << log;
}

// kill -9 of the replica, the master frozen, the replica restarted: what it serves is what it recovered,
// since it cannot reload from a stopped master. The caller resumes the master.
std::unique_ptr<RespClient> Kill9AndRestartAgainstAFrozenMaster(TestServer& r, TestServer& m) {
  r.Kill9();
  m.Pause();
  r.Restart(ReplicaOfArgs(m));
  return Client(r);
}

void ExpectDiscarded(TestServer& r, RespClient& rc) {
  EXPECT_NE(r.StderrSinceStart().find("interrupted full synchronisation"), std::string::npos)
      << r.StderrSinceStart();
  EXPECT_EQ(rc.Command({"DBSIZE"}).integer, 0);
  EXPECT_EQ(Digest(rc), kZeroDigest);
  EXPECT_EQ(rc.Command({"EXISTS", "standalone-key"}).integer, 0);  // wiped by the start txn, not revived
}

// Crash point 1: the start txn (wipe + marker) committed, no chunk yet. The master's rdb-key-save-delay
// holds the first chunk back far longer than the test runs.
TEST(FullSync, ReplicaKill9BetweenTheStartTxnAndTheFirstChunkRestartsEmpty) {
  TestServer m(TIDESKV_SERVER_BIN);
  auto mc = Client(m);
  Fill(*mc, 2000, 10);
  if (HasFatalFailure()) return;
  ASSERT_EQ(mc->Command({"CONFIG", "SET", "rdb-key-save-delay", "100000"}).str, "OK");  // 100 ms per key
  TempDir dir("tideskv-l5-start");
  TestServer r(TIDESKV_SERVER_BIN, {.dir = dir.str()});
  {
    auto rc = Client(r);
    ASSERT_TRUE(ForceFullSync(*rc));
    Follow(r, m);
    ASSERT_TRUE(WaitFor([&] { return LoadStarted(*rc); }, 10s));
    ASSERT_EQ(InfoField(*rc, "persistence", "loading_loaded_bytes"), 0) << "a chunk already arrived";
  }
  auto rc = Kill9AndRestartAgainstAFrozenMaster(r, m);
  ASSERT_NE(rc, nullptr);
  ExpectDiscarded(r, *rc);
  m.Resume();
  ASSERT_EQ(mc->Command({"CONFIG", "SET", "rdb-key-save-delay", "0"}).str, "OK");
  ASSERT_TRUE(WaitConnected(r, 120s));
  ExpectConverged(m, {&r});
  ExpectNoSanitizerReport(r);
  ExpectNoSanitizerReport(m);
}

// Crash point 2: a replica killed between chunks restarts with the partial keyspace discarded (never
// served), then reloads and converges.
TEST(FullSync, ReplicaKill9DuringLoadNeverServesAPartialKeyspace) {
  TestServer m(TIDESKV_SERVER_BIN);
  auto mc = Client(m);
  Fill(*mc, kKeys, kValueBytes);
  if (HasFatalFailure()) return;
  TempDir dir("tideskv-l5-fullsync");
  TestServer r(TIDESKV_SERVER_BIN, {.extra_args = {"--key-load-delay", "200"}, .dir = dir.str()});
  {
    auto rc = Client(r);
    ASSERT_TRUE(ForceFullSync(*rc));
    Follow(r, m);
    // A chunk is committed (loaded_bytes is published after it), and more are to come.
    ASSERT_TRUE(WaitFor([&] { return InfoField(*rc, "persistence", "loading_loaded_bytes") > 0; }, 20s));
    EXPECT_EQ(rc->Command({"DBSIZE"}).str, "LOADING Redis is loading the dataset in memory");
  }
  auto rc = Kill9AndRestartAgainstAFrozenMaster(r, m);
  ASSERT_NE(rc, nullptr);
  ExpectDiscarded(r, *rc);
  m.Resume();
  ASSERT_TRUE(WaitConnected(r, 120s));
  ExpectConverged(m, {&r});
  ExpectNoSanitizerReport(r);
  ExpectNoSanitizerReport(m);
}

// Crash point 3: the end txn committed but the master never saw an ACK. The snapshot (one chunk) fits in
// the socket buffers, so the master has written all of it, LOADEND included, long before the replica's
// key-load-delay lets it commit the chunk; the master is frozen before that, so the replica's first ACK
// stays unread. The restart serves the complete keyspace and continues from S + 1.
TEST(FullSync, ReplicaKill9AfterTheEndTxnBeforeAnAckKeepsTheCompleteKeyspace) {
  constexpr int kSmall = 3000;  // one chunk of about 60 KB: 3000 keys x 2 ms of key-load-delay = 6 s
  TestServer m(TIDESKV_SERVER_BIN);
  auto mc = Client(m);
  Fill(*mc, kSmall, 10);
  if (HasFatalFailure()) return;
  const std::string digest = Digest(*mc);
  TempDir dir("tideskv-l5-end");
  TestServer r(TIDESKV_SERVER_BIN, {.extra_args = {"--key-load-delay", "2000"}, .dir = dir.str()});
  auto rc = Client(r);
  ASSERT_TRUE(ForceFullSync(*rc));
  Follow(r, m);
  ASSERT_TRUE(WaitFor([&] { return LoadStarted(*rc); }, 10s));
  std::this_thread::sleep_for(1s);  // the master writes ~60 KB in milliseconds; the replica needs 6 s
  ASSERT_TRUE(Loading(*rc)) << "the load ended before the master was frozen";
  m.Pause();
  ASSERT_TRUE(WaitFor([&] { return !Loading(*rc); }, 60s));
  ASSERT_EQ(rc->Command({"DBSIZE"}).integer, kSmall) << "the end txn did not commit";
  rc.reset();
  r.Kill9();
  r.Restart(ReplicaOfArgs(m));
  rc = Client(r);
  ASSERT_NE(rc, nullptr);
  EXPECT_EQ(r.StderrSinceStart().find("interrupted full synchronisation"), std::string::npos)
      << r.StderrSinceStart();
  EXPECT_EQ(rc->Command({"DBSIZE"}).integer, kSmall);
  EXPECT_EQ(Digest(*rc), digest);
  m.Resume();
  ASSERT_TRUE(WaitConnected(r, 120s));
  ExpectConverged(m, {&r});
  EXPECT_EQ(InfoField(*mc, "stats", "sync_full"), 1);  // the restart continued from the loaded baseline
  EXPECT_GE(InfoField(*mc, "stats", "sync_partial_ok"), 1);
}

// A master killed mid-transfer: the replica discards its partial load; the master restarts (new replid,
// R12) and the replica reloads or continues from an empty seq-0 baseline, then converges.
TEST(FullSync, MasterKill9DuringTransferThenConverges) {
  const uint32_t seed = Seed();
  SCOPED_TRACE("seed " + std::to_string(seed));
  TempDir dir("tideskv-l5-master");
  TestServer m(TIDESKV_SERVER_BIN, {.extra_args = {"--sync-mode", "full"}, .dir = dir.str()});
  auto mc = Client(m);
  Fill(*mc, kKeys, kValueBytes);
  if (HasFatalFailure()) return;
  ASSERT_EQ(mc->Command({"CONFIG", "SET", "rdb-key-save-delay", "200"}).str, "OK");  // ~0.8 s per chunk
  TestServer r(TIDESKV_SERVER_BIN);
  auto rc = Client(r);
  ASSERT_TRUE(ForceFullSync(*rc));
  Follow(r, m);
  ASSERT_TRUE(WaitFor([&] { return InfoField(*rc, "persistence", "loading_loaded_bytes") > 0; }, 20s));
  Workload load(m.port(), seed, 2);
  load.Start();
  std::this_thread::sleep_for(300ms);
  mc.reset();
  m.Kill9();
  load.Stop();
  ReportLoad(load);
  ASSERT_TRUE(WaitFor([&] { return !Loading(*rc); }, 70s));
  EXPECT_EQ(rc->Command({"DBSIZE"}).integer, 0);
  EXPECT_EQ(Digest(*rc), kZeroDigest);
  m.Restart({"--sync-mode", "full"});  // rdb-key-save-delay was runtime-only: back to 0
  ASSERT_TRUE(WaitConnected(r, 120s));
  ExpectConverged(m, {&r});
}

// Repeated kills of a replica that must full-sync every time: a 1 KiB backlog trims past it while it is
// down, and a discarded load restarts empty against a trimmed log. key-load-delay stretches each load so
// that kills land inside loads as well as between them.
TEST(FullSync, Kill9LoopOfAReplicaThatFullSyncsEachTime) {
  const uint32_t seed = Seed();
  SCOPED_TRACE("seed " + std::to_string(seed));
  std::mt19937 rng(seed);
  TestServer m(TIDESKV_SERVER_BIN, {.extra_args = {"--repl-backlog-size", "1024"}});
  auto mc = Client(m);
  Fill(*mc, 5000, 10);
  if (HasFatalFailure()) return;
  ASSERT_TRUE(
      WaitFor([&] { return InfoField(*mc, "replication", "repl_backlog_first_byte_offset") > 1; }, 10s));
  TempDir dir("tideskv-l5-loop");
  std::vector<std::string> args = ReplicaOfArgs(m);
  args.insert(args.end(), {"--key-load-delay", "100"});  // 5000 keys: about 0.5 s per load
  TestServer r(TIDESKV_SERVER_BIN, {.extra_args = args, .dir = dir.str()});
  Workload load(m.port(), seed, 2);
  load.Start();
  for (int round = 0; round < 5; ++round) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200 + rng() % 1300));
    r.Kill9();
    std::this_thread::sleep_for(1500ms);  // writes trim the 1 KiB backlog past the replica
    r.Restart();
  }
  load.Stop();
  ReportLoad(load);
  ASSERT_TRUE(WaitConnected(r, 120s));
  ExpectConverged(m, {&r});
  EXPECT_GE(InfoField(*mc, "stats", "sync_full"), 5);
}

}  // namespace
}  // namespace tideskv::test
