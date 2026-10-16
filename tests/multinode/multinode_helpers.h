#pragma once
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "multinode/workload.h"
#include "support/it_helpers.h"
#include "support/test_server.h"

// Shared by the multi-node suites (M6a Task 15): node roles and links, seeds, and the convergence check.
namespace tideskv::test::multinode {

using namespace std::chrono_literals;

inline std::unique_ptr<RespClient> Client(const TestServer& s) {
  auto c = s.Connect();
  EXPECT_NE(c, nullptr) << "cannot connect to port " << s.port();
  return c;
}

inline std::string Digest(RespClient& c) { return c.Command({"DEBUG", "DIGEST"}).str; }

// DEBUG DIGEST-INDEX (M7a D23): [hex, entries, missing, orphans].
inline RespValue DigestIndex(RespClient& c) { return c.Command({"DEBUG", "DIGEST-INDEX"}); }

inline int64_t Sequence(RespClient& c) { return InfoField(c, "keyspace", "sequence"); }

// ROLE's link state on a replica ("connect", "connecting", "handshake", "connected"); "" on a master.
inline std::string LinkState(RespClient& c) {
  const RespValue r = c.Command({"ROLE"});
  return r.type == RespValue::Type::kArray && r.elements.size() == 5 ? r.elements[3].str : "";
}

inline std::vector<std::string> ReplicaOfArgs(const TestServer& master) {
  return {"--replicaof", "127.0.0.1 " + std::to_string(master.port())};  // CF11: one argument
}

inline void Follow(const TestServer& replica, const TestServer& master) {
  auto c = Client(replica);
  ASSERT_NE(c, nullptr);
  ASSERT_EQ(c->Command({"REPLICAOF", "127.0.0.1", std::to_string(master.port())}).str, "OK");
}

inline bool WaitConnected(const TestServer& replica, std::chrono::milliseconds timeout = 30s) {
  auto c = Client(replica);
  return c != nullptr && WaitFor([&] { return LinkState(*c) == "connected"; }, timeout);
}

// TIDESKV_MULTINODE_SEED reproduces a run; otherwise a fresh seed, printed so a failure can be replayed.
inline uint32_t Seed() {
  const char* env = std::getenv("TIDESKV_MULTINODE_SEED");
  const uint32_t seed =
      env != nullptr ? static_cast<uint32_t>(std::strtoul(env, nullptr, 10)) : std::random_device{}();
  std::printf("[ multinode ] seed %u (rerun with TIDESKV_MULTINODE_SEED=%u)\n", seed, seed);
  return seed;
}

// Prints what a load did: steps, clients that ended early, and every error reply by text.
inline void ReportLoad(const Workload& load) {
  const Workload::Stats st = load.stats();
  std::printf("[ multinode ] load: %llu steps, %llu acknowledged, %llu early exits, %llu errors\n",
              static_cast<unsigned long long>(st.steps), static_cast<unsigned long long>(load.acked_writes()),
              static_cast<unsigned long long>(st.early_exits),
              static_cast<unsigned long long>(st.error_count()));
  for (const auto& [text, n] : st.errors) {
    std::printf("[ multinode ]   %llu x %s\n", static_cast<unsigned long long>(n), text.c_str());
  }
}

// For a load whose server stayed up: every client ran until Stop(), and the only error replies are the
// ones the mix makes by design (RENAME of a key that does not exist, about 1% of the steps), at a bounded
// rate. Prints the load first.
inline void ExpectHealthyLoad(const Workload& load) {
  ReportLoad(load);
  const Workload::Stats st = load.stats();
  EXPECT_GT(st.steps, 0u);
  EXPECT_EQ(st.early_exits, 0u) << "a client lost its connection or timed out";
  for (const auto& [text, n] : st.errors) EXPECT_EQ(text, "ERR no such key") << n << " replies";
  EXPECT_LE(st.error_count() * 10, st.steps) << "more than 10% of the steps failed";
}

// Waits until every replica has applied the master's last seq, then compares DEBUG DIGEST and DEBUG
// DIGEST-INDEX (M7a D23: TTL deadlines and the expire index; no node may miss an expire entry). The master
// may still write after the load stops (its reaper deletes expired keys), so a snapshot counts only if
// the master's sequence did not move while the digests were read; a stable snapshot whose digests differ
// is a divergence and fails at once.
inline void ExpectConverged(const TestServer& master, const std::vector<const TestServer*>& replicas,
                            std::chrono::milliseconds timeout = 60s) {
  auto m = Client(master);
  ASSERT_NE(m, nullptr);
  std::vector<std::unique_ptr<RespClient>> rs;
  for (const TestServer* r : replicas) {
    rs.push_back(Client(*r));
    ASSERT_NE(rs.back(), nullptr);
  }
  std::string report;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const int64_t seq = Sequence(*m);
    bool caught_up = true;
    report = "master seq " + std::to_string(seq);
    for (size_t i = 0; i < rs.size(); ++i) {
      const int64_t rseq = Sequence(*rs[i]);
      report +=
          ", replica" + std::to_string(i) + " seq " + std::to_string(rseq) + " link " + LinkState(*rs[i]);
      caught_up = caught_up && rseq == seq;
    }
    if (caught_up) {
      const std::string digest = Digest(*m);
      const RespValue index = DigestIndex(*m);
      std::vector<std::string> digests;
      std::vector<RespValue> indexes;
      for (auto& r : rs) {
        digests.push_back(Digest(*r));
        indexes.push_back(DigestIndex(*r));
      }
      if (Sequence(*m) == seq) {
        ASSERT_EQ(index.elements.size(), 4u) << ToDebugString(index);
        EXPECT_EQ(index.elements[2].integer, 0) << "the master's expire index lost entries";
        for (size_t i = 0; i < rs.size(); ++i) {
          EXPECT_EQ(digests[i], digest) << "replica" << i << " diverged at seq " << seq;
          ASSERT_EQ(indexes[i].elements.size(), 4u) << ToDebugString(indexes[i]);
          EXPECT_EQ(indexes[i].elements[0].str, index.elements[0].str)
              << "replica" << i << " expire index diverged at seq " << seq;
          EXPECT_EQ(indexes[i].elements[2].integer, 0) << "replica" << i << " lost expire entries";
        }
        EXPECT_NE(digest, std::string(40, '0')) << "the master is empty: the load wrote nothing";
        return;
      }
    }
    std::this_thread::sleep_for(50ms);
  }
  ADD_FAILURE() << "replicas did not catch up within " << timeout.count() << " ms: " << report;
}

}  // namespace tideskv::test::multinode
