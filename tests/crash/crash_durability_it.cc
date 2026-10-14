// D1: with concurrent writers and frequent memtable rotations, kill -9 must not lose or tear an
// acknowledged write (TidesDB 10.1.1 split-apply race; notes/m6-research/tidesdb-replication.md §5.2).
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "support/test_server.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace tideskv::test {
namespace {

constexpr int kWriters = 8;
constexpr int kDefaultRounds = 20;
constexpr int kKeysPerTxn = 32;

// TIDESKV_CRASH_ROUNDS shortens the run in CI; anything that is not a positive integer keeps the default.
int Rounds() {
  const char* env = std::getenv("TIDESKV_CRASH_ROUNDS");
  if (env == nullptr) return kDefaultRounds;
  char* end = nullptr;
  const long parsed = std::strtol(env, &end, 10);
  if (end == env || *end != '\0' || parsed < 1 || parsed > 1000) return kDefaultRounds;
  return static_cast<int>(parsed);
}

using AckedIndexes = std::vector<std::vector<int>>;  // per writer: acknowledged indexes, in order

std::string KeyOf(int writer, int txn, int key) {
  return "w:" + std::to_string(writer) + ":" + std::to_string(txn) + ":" + std::to_string(key);
}

// Each writer commits one MSET of kKeysPerTxn keys per txn, so a torn txn is detectable.
AckedIndexes RunWritersUntilKilled(TestServer& srv, std::chrono::milliseconds run_for) {
  AckedIndexes acked(kWriters);
  std::vector<std::unique_ptr<RespClient>> clients;
  clients.reserve(kWriters);
  for (int w = 0; w < kWriters; ++w) clients.push_back(srv.Connect());
  std::vector<std::thread> threads;
  threads.reserve(kWriters);
  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&acked, &clients, w] {
      RespClient& c = *clients[w];
      const std::string value(256, 'x');  // fills the 1 MiB memtable fast: frequent rotations
      for (int i = 0;; ++i) {
        std::vector<std::string> mset = {"MSET"};
        mset.reserve(1 + 2 * kKeysPerTxn);
        for (int j = 0; j < kKeysPerTxn; ++j) {
          mset.push_back(KeyOf(w, i, j));
          mset.push_back(value);
        }
        // not MULTI/EXEC: EXEC runs exclusively, so it would never overlap another commit
        if (!c.Send(mset)) return;
        std::optional<RespValue> reply = c.Read();
        if (!reply || reply->type != RespValue::Type::kSimple) return;  // killed mid-command: not acked
        acked[w].push_back(i);
      }
    });
  }
  std::this_thread::sleep_for(run_for);
  srv.Kill9();
  for (auto& t : threads) t.join();
  return acked;
}

void RunCrashRound(const std::string& sync_mode, int round) {
  TempDir dir("tideskv-crash");
  AckedIndexes acked;
  {
    TestServer srv(TIDESKV_SERVER_BIN,
                   {.extra_args = {"--sync-mode", sync_mode, "--memtable-size", "1mb", "--workers", "8"},
                    .dir = dir.str()});
    acked = RunWritersUntilKilled(srv, std::chrono::milliseconds(400 + 37 * round));
  }
  TestServer again(TIDESKV_SERVER_BIN, {.extra_args = {"--sync-mode", sync_mode}, .dir = dir.str()});
  auto c = again.Connect();
  ASSERT_NE(c, nullptr);
  size_t n_acked = 0;
  for (auto& a : acked) n_acked += a.size();
  ASSERT_GT(n_acked, 0u) << sync_mode << " round " << round << ": no write was acknowledged before the kill";
  for (size_t w = 0; w < acked.size(); ++w) {
    for (int i : acked[w]) {
      std::vector<std::string> exists = {"EXISTS"};
      for (int j = 0; j < kKeysPerTxn; ++j) exists.push_back(KeyOf(static_cast<int>(w), i, j));
      ASSERT_EQ(c->Command(exists).integer, kKeysPerTxn)
          << sync_mode << " round " << round << ": acknowledged txn lost or torn: writer " << w << " txn "
          << i;
    }
  }
}

TEST(CrashDurability, ConcurrentWritersSurviveKill9UnderFull) {
  for (int round = 0; round < Rounds(); ++round) RunCrashRound("full", round);
}

TEST(CrashDurability, ConcurrentWritersSurviveKill9UnderInterval) {
  for (int round = 0; round < Rounds(); ++round) RunCrashRound("interval", round);
}

}  // namespace
}  // namespace tideskv::test
