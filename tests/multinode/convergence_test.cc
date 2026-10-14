#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "multinode/multinode_helpers.h"
#include "multinode/workload.h"
#include "support/temp_dir.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

// L5 (M6a exit criterion): replicas converge on the master's DEBUG DIGEST under a mixed write load,
// across kill -9 of the replica and of the master.
namespace tideskv::test {
namespace {

using namespace multinode;

constexpr int kClients = 4;

std::chrono::milliseconds RandomMs(std::mt19937& rng, int lo, int hi) {
  return std::chrono::milliseconds(std::uniform_int_distribution<int>(lo, hi)(rng));
}

TEST(Convergence, ReplicaMatchesMasterUnderLoad) {
  const uint32_t seed = Seed();
  SCOPED_TRACE("seed " + std::to_string(seed));
  TestServer master(TIDESKV_SERVER_BIN);
  TestServer r1(TIDESKV_SERVER_BIN, {.extra_args = ReplicaOfArgs(master)});
  TestServer r2(TIDESKV_SERVER_BIN, {.extra_args = ReplicaOfArgs(master)});
  ASSERT_TRUE(WaitConnected(r1));
  ASSERT_TRUE(WaitConnected(r2));
  Workload load(master.port(), seed, kClients);
  load.Start();
  std::this_thread::sleep_for(10s);
  load.Stop();
  std::printf("[ multinode ] %llu writes acknowledged\n",
              static_cast<unsigned long long>(load.acked_writes()));
  EXPECT_GT(load.acked_writes(), 1000u);
  ExpectConverged(master, {&r1, &r2});
  // The TSan smoke runs this test against a -fsanitize=thread server: a race report fails it here, with
  // the server's log, and not only through the exit code.
  for (TestServer* s : {&r1, &r2, &master}) {
    EXPECT_EQ(s->Terminate(), 0) << s->Stderr();
    EXPECT_EQ(s->Stderr().find("ThreadSanitizer"), std::string::npos) << s->Stderr();
  }
}

// The master stays up; every restart of the replica must resume with a partial resync.
TEST(Convergence, ReplicaKill9Loop) {
  const uint32_t seed = Seed();
  SCOPED_TRACE("seed " + std::to_string(seed));
  std::mt19937 rng(seed);
  TestServer master(TIDESKV_SERVER_BIN);
  TempDir replica_dir("tideskv-mn-replica");
  TestServer replica(TIDESKV_SERVER_BIN, {.extra_args = ReplicaOfArgs(master), .dir = replica_dir.str()});
  ASSERT_TRUE(WaitConnected(replica));
  Workload load(master.port(), seed, kClients);
  load.Start();
  for (int round = 0; round < 10; ++round) {
    std::this_thread::sleep_for(RandomMs(rng, 200, 1500));
    replica.Kill9();
    replica.Restart();
    ASSERT_TRUE(replica.IsRunning()) << "round " << round << ":\n" << replica.StderrSinceStart();
  }
  load.Stop();
  ExpectConverged(master, {&replica});
  auto m = Client(master);
  // The first link plus one per restart; a restart that came up before the old link was reaped still
  // counts once.
  EXPECT_GE(InfoField(*m, "stats", "sync_partial_ok"), 10);
  EXPECT_EQ(InfoField(*m, "stats", "sync_full"), 0);
}

// Both nodes fsync every commit (D1), so the master never loses a seq the replica already applied, nor a
// write a client saw acknowledged.
TEST(Convergence, MasterKill9Loop) {
  const uint32_t seed = Seed();
  SCOPED_TRACE("seed " + std::to_string(seed));
  std::mt19937 rng(seed);
  const std::vector<std::string> full = {"--sync-mode", "full"};
  TempDir master_dir("tideskv-mn-master");
  TestServer master(TIDESKV_SERVER_BIN, {.extra_args = full, .dir = master_dir.str()});
  std::vector<std::string> replica_args = full;
  for (const std::string& a : ReplicaOfArgs(master)) replica_args.push_back(a);
  TestServer replica(TIDESKV_SERVER_BIN, {.extra_args = replica_args});
  ASSERT_TRUE(WaitConnected(replica));
  Workload load(master.port(), seed, kClients);
  for (int round = 0; round < 5; ++round) {
    load.Start();
    std::this_thread::sleep_for(RandomMs(rng, 300, 1500));
    master.Kill9();
    load.Stop();  // its clients see EOF and end
    master.Restart();
    ASSERT_TRUE(master.IsRunning()) << "round " << round << ":\n" << master.StderrSinceStart();
    // The restarted master's counters start at zero: one CONTINUE proves the replica's new link.
    auto m = Client(master);
    ASSERT_TRUE(WaitFor([&] { return InfoField(*m, "stats", "sync_partial_ok") == 1; }, 30s))
        << "round " << round << ": the replica did not resync\n"
        << replica.Stderr();
    ASSERT_TRUE(WaitConnected(replica)) << "round " << round;
    EXPECT_EQ(InfoField(*m, "stats", "sync_full"), 0) << "round " << round;
  }
  load.Start();
  std::this_thread::sleep_for(1s);
  load.Stop();
  ExpectConverged(master, {&replica});
  const std::vector<uint64_t> acked = load.acked_ack_keys();
  ASSERT_FALSE(acked.empty());
  auto m = Client(master);
  for (uint64_t n : acked) {
    ASSERT_EQ(m->Command({"EXISTS", "ack:" + std::to_string(n)}).integer, 1)
        << "acknowledged SET ack:" << n << " was lost by the master's kill -9";
  }
}

}  // namespace
}  // namespace tideskv::test
