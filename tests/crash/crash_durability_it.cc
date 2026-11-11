// D1: with concurrent writers and frequent memtable rotations, kill -9 must not lose or tear an
// acknowledged write (TidesDB 10.1.1 split-apply race; notes/m6-research/tidesdb-replication.md §5.2).
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "support/it_helpers.h"
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

// Loops HSET crash:h:<n> f v / DEL crash:h:<n> until the server dies. Each DEL is a collection range delete
// that the compaction scheduler counts; every job it runs checkpoints, i.e. forces a memtable rotation.
void ChurnCollections(RespClient& c) {
  for (int n = 0;; ++n) {
    const std::string key = "crash:h:" + std::to_string(n);
    if (!c.Send({"HSET", key, "f", "v"}) || !c.Read()) return;
    if (!c.Send({"DEL", key}) || !c.Read()) return;
  }
}

struct RoundOptions {
  std::vector<std::string> extra_args;  // added to the first server's arguments
  bool churn = false;                   // a 9th client drives the compaction scheduler (ChurnCollections)
  int64_t* background_compactions = nullptr;  // when set, INFO's count just before the kill is added to it
};

// Each writer commits one MSET of kKeysPerTxn keys per txn, so a torn txn is detectable.
AckedIndexes RunWritersUntilKilled(TestServer& srv, std::chrono::milliseconds run_for,
                                   const RoundOptions& opts) {
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
  std::unique_ptr<RespClient> churner = opts.churn ? srv.Connect() : nullptr;
  if (churner) threads.emplace_back([&churner] { ChurnCollections(*churner); });
  std::unique_ptr<RespClient> info = opts.background_compactions != nullptr ? srv.Connect() : nullptr;
  std::this_thread::sleep_for(run_for);
  if (info) {  // the count dies with the server: read it just before the kill
    *opts.background_compactions +=
        std::max<int64_t>(0, InfoField(*info, "persistence", "background_compactions"));
  }
  srv.Kill9();
  for (auto& t : threads) t.join();
  return acked;
}

void RunCrashRound(const std::string& sync_mode, int round, const RoundOptions& opts = {}) {
  TempDir dir("tideskv-crash");
  AckedIndexes acked;
  {
    std::vector<std::string> args = {"--sync-mode", sync_mode, "--memtable-size", "1mb", "--workers", "8"};
    args.insert(args.end(), opts.extra_args.begin(), opts.extra_args.end());
    TestServer srv(TIDESKV_SERVER_BIN, {.extra_args = args, .dir = dir.str()});
    acked = RunWritersUntilKilled(srv, std::chrono::milliseconds(400 + 37 * round), opts);
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

// As the FULL test, with the compaction scheduler checkpointing (forcing a rotation) for every job while
// the writers run. The jobs must actually have run, or the test proves nothing about forced rotations.
TEST(CrashDurability, ConcurrentWritersSurviveKill9UnderFullWithForcedRotations) {
  int64_t background_compactions = 0;
  const RoundOptions opts = {
      .extra_args = {"--compaction-collection-deletes", "1", "--compaction-collection-dead-ratio", "0"},
      .churn = true,
      .background_compactions = &background_compactions};
  for (int round = 0; round < Rounds(); ++round) {
    RunCrashRound("full", round, opts);
    if (HasFatalFailure()) return;
  }
  std::printf("[forced rotations] background_compactions over %d rounds: %lld\n", Rounds(),
              static_cast<long long>(background_compactions));
  EXPECT_GT(background_compactions, 0) << "no compaction scheduler job ran, so no forced rotation was tested";
}

TEST(CrashDurability, ConcurrentWritersSurviveKill9UnderInterval) {
  for (int round = 0; round < Rounds(); ++round) RunCrashRound("interval", round);
}

}  // namespace
}  // namespace tideskv::test
