#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "support/it_helpers.h"
#include "support/temp_dir.h"
#include "support/test_server.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace tideskv::test {
namespace {

using namespace std::chrono_literals;

// Sends MULTI, the commands and EXEC in one write; returns EXEC's reply.
RespValue Transaction(RespClient& c, const std::vector<std::vector<std::string>>& cmds) {
  std::string wire = EncodeCommand({"MULTI"});
  for (const auto& cmd : cmds) wire += EncodeCommand(cmd);
  wire += EncodeCommand({"EXEC"});
  EXPECT_TRUE(c.SendRaw(wire));
  for (size_t i = 0; i < cmds.size() + 1; ++i) EXPECT_TRUE(c.Read().has_value());  // +OK, +QUEUED ...
  auto r = c.Read();
  EXPECT_TRUE(r.has_value());
  return r.value_or(RespValue{});
}

// Stops and joins the test's threads on every exit path, so a failed ASSERT cannot reach
// std::terminate through a joinable std::thread.
struct ThreadGuard {
  std::atomic<bool>& stop;
  std::vector<std::thread>& threads;
  ~ThreadGuard() {
    stop = true;
    for (auto& t : threads) {
      if (t.joinable()) t.join();
    }
  }
};

int64_t Pttl(RespClient& c, const std::string& key) { return c.Command({"PTTL", key}).integer; }

// The integer stored at `key` (0 when missing).
int64_t IntValue(RespClient& c, const std::string& key) {
  const RespValue v = c.Command({"GET", key});
  return v.type == RespValue::Type::kBulk ? std::stoll(v.str) : 0;
}

TEST(MultiIT, ExecDeletingFortyCollectionsCommits) {  // D2: TidesDB's 32-range-per-commit limit
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  std::vector<std::vector<std::string>> dels;
  for (int i = 0; i < 40; ++i) {
    ASSERT_EQ(c->Command({"HSET", "h" + std::to_string(i), "f", "v"}).integer, 1);
    dels.push_back({"DEL", "h" + std::to_string(i)});
  }
  const RespValue r = Transaction(*c, dels);
  ASSERT_EQ(r.type, RespValue::Type::kArray);
  ASSERT_EQ(r.elements.size(), 40U);
  for (const auto& e : r.elements) EXPECT_EQ(e.integer, 1);
  EXPECT_EQ(c->Command({"DBSIZE"}).integer, 0);
}

TEST(MultiIT, ExecIsIsolatedFromConcurrentWriters) {  // Review Focus #2
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--workers", "4"}});
  std::atomic<bool> stop{false};
  std::vector<std::thread> writers;
  const ThreadGuard guard{stop, writers};
  for (int t = 0; t < 4; ++t) {
    writers.emplace_back([&] {
      auto c = s.Connect();
      while (c && !stop) c->Command({"INCR", "k"});
    });
  }
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  for (int i = 0; i < 300; ++i) {
    const RespValue r = Transaction(*c, {{"GET", "k"}, {"GET", "k"}});
    ASSERT_EQ(r.type, RespValue::Type::kArray);
    ASSERT_EQ(r.elements.size(), 2u);
    EXPECT_EQ(r.elements[0].str, r.elements[1].str) << "iteration " << i;
  }
}

TEST(MultiIT, ReadersNeverSeeHalfAnExec) {  // Review Focus #2
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--workers", "4"}});
  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;
  const ThreadGuard guard{stop, threads};
  threads.emplace_back([&] {
    auto c = s.Connect();
    while (c && !stop) Transaction(*c, {{"INCR", "a"}, {"INCR", "b"}});
  });
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  for (int i = 0; i < 2000; ++i) {
    const RespValue r = c->Command({"MGET", "a", "b"});
    ASSERT_EQ(r.elements.size(), 2u);
    EXPECT_EQ(r.elements[0].str, r.elements[1].str);
  }
  stop = true;
  threads.front().join();
  EXPECT_GT(IntValue(*c, "a"), 0) << "the writer made no progress";
}

TEST(MultiIT, WatchCasLoopIsExact) {  // Review Focus #1
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--workers", "4"}});
  constexpr int kClients = 8;
  constexpr int kIncrements = 50;
  std::atomic<int> errors{0};
  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;
  const ThreadGuard guard{stop, threads};
  const auto deadline = std::chrono::steady_clock::now() + 60s;
  for (int t = 0; t < kClients; ++t) {
    threads.emplace_back([&] {
      auto c = s.Connect();
      if (!c) return void(++errors);
      for (int done = 0; done < kIncrements;) {
        if (std::chrono::steady_clock::now() > deadline) return void(++errors);
        c->Command({"WATCH", "ctr"});
        const RespValue v = c->Command({"GET", "ctr"});
        if (v.type != RespValue::Type::kNull && v.type != RespValue::Type::kBulk) return void(++errors);
        const int64_t n = v.type == RespValue::Type::kNull ? 0 : std::stoll(v.str);
        const RespValue r = Transaction(*c, {{"SET", "ctr", std::to_string(n + 1)}});
        if (r.type == RespValue::Type::kArray) {
          ++done;
        } else if (r.type != RespValue::Type::kNull) {  // null: lost the race, retry
          ++errors;
          return;
        }
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(errors.load(), 0);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"GET", "ctr"}).str, std::to_string(kClients * kIncrements));
}

TEST(MultiIT, KillDuringExecIsAllOrNothing) {  // Review Focus #2; spec §12 "kill"
  TempDir dir("tideskv-it-multi-kill");
  std::mt19937 rng(std::random_device{}());
  for (int round = 0; round < 5; ++round) {
    {
      TestServer s(TIDESKV_SERVER_BIN, {.dir = dir.str()});
      std::atomic<bool> stop{false};
      std::vector<std::thread> threads;
      const ThreadGuard guard{stop, threads};
      auto probe = s.Connect();
      ASSERT_NE(probe, nullptr);
      // The data dir is shared across rounds, so progress is measured from this round's start.
      const int64_t baseline = IntValue(*probe, "a");
      threads.emplace_back([&] {
        auto c = s.Connect();
        for (int i = 0; c && !stop; ++i) {
          const std::string id = std::to_string(round) + ":" + std::to_string(i);
          const std::string wire = EncodeCommand({"MULTI"}) + EncodeCommand({"INCR", "a"}) +
                                   EncodeCommand({"INCR", "b"}) + EncodeCommand({"SADD", "s", id}) +
                                   EncodeCommand({"LPUSH", "l", id}) + EncodeCommand({"EXEC"});
          if (!c->SendRaw(wire)) return;
          for (int r = 0; r < 6; ++r) {
            if (!c->Read()) return;  // the server died
          }
        }
      });
      ASSERT_TRUE(WaitFor([&] { return IntValue(*probe, "a") >= baseline + 5; })) << "round " << round;
      std::this_thread::sleep_for(std::chrono::milliseconds(rng() % 200));
      s.Kill9();
      stop = true;
      threads.front().join();
    }
    TestServer s(TIDESKV_SERVER_BIN, {.dir = dir.str()});
    auto c = s.Connect();
    ASSERT_NE(c, nullptr);
    const std::string a = c->Command({"GET", "a"}).str;
    const std::string n = a.empty() ? "0" : a;
    EXPECT_EQ(c->Command({"GET", "b"}).str, a) << "round " << round;
    EXPECT_EQ(std::to_string(c->Command({"SCARD", "s"}).integer), n) << "round " << round;
    EXPECT_EQ(std::to_string(c->Command({"LLEN", "l"}).integer), n) << "round " << round;
  }
}

TEST(MultiIT, WaiterIsServedAfterExecWithTheFinalState) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto w = s.Connect();
  auto c = s.Connect();
  ASSERT_TRUE(w && c);
  ASSERT_TRUE(w->Send({"BLPOP", "q", "0"}));
  ASSERT_TRUE(WaitFor([&] { return InfoField(*c, "clients", "blocked_clients") == 1; }));
  const RespValue r1 = Transaction(*c, {{"LPUSH", "q", "x"}, {"LPOP", "q"}});
  ASSERT_EQ(r1.elements.size(), 2u);
  EXPECT_EQ(r1.elements[1].str, "x");
  EXPECT_EQ(InfoField(*c, "clients", "blocked_clients"), 1);  // nothing was left for it
  Transaction(*c, {{"LPUSH", "q", "y"}});
  const auto got = w->Read();
  ASSERT_TRUE(got.has_value());
  ASSERT_EQ(got->elements.size(), 2u);
  EXPECT_EQ(got->elements[1].str, "y");
}

TEST(MultiIT, ClientUnblockInsideExecRace) {  // Review Focus #5, G4
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--workers", "4"}});
  auto w = s.Connect();
  auto pusher = s.Connect();
  auto execer = s.Connect();
  ASSERT_TRUE(w && pusher && execer);
  const std::string wid = std::to_string(w->Command({"CLIENT", "ID"}).integer);
  for (int i = 0; i < 200; ++i) {
    ASSERT_TRUE(w->Send({"BLPOP", "q", "0"}));
    ASSERT_TRUE(WaitFor([&] { return InfoField(*execer, "clients", "blocked_clients") == 1; }));
    std::thread p([&] { pusher->Command({"LPUSH", "q", "v"}); });
    const RespValue r = Transaction(*execer, {{"CLIENT", "UNBLOCK", wid}, {"SET", "z", "1"}});
    p.join();
    EXPECT_EQ(r.type, RespValue::Type::kArray);
    const auto reply = w->Read();  // the element or a null, never a hang
    ASSERT_TRUE(reply.has_value()) << "iteration " << i;
    const int64_t left = pusher->Command({"LLEN", "q"}).integer;
    if (reply->type == RespValue::Type::kArray) {
      ASSERT_EQ(reply->elements.size(), 2u) << "iteration " << i;
      EXPECT_EQ(reply->elements[1].str, "v") << "iteration " << i;
      EXPECT_EQ(left, 0) << "iteration " << i;  // the pushed element was consumed
    } else {
      EXPECT_EQ(reply->type, RespValue::Type::kNull) << "iteration " << i;
      EXPECT_EQ(left, 1) << "iteration " << i;  // unblocked first: the element stays
    }
    pusher->Command({"DEL", "q"});
  }
}

// The lazy, not-yet-reaped path is covered by the unit test
// MultiExpiry.LiveWatchedKeyThatExpiresBeforeExecAborts; here the key may also be reaped.
TEST(MultiIT, WatchedKeyExpiringBeforeExecAborts) {  // Review Focus #4
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  for (int attempt = 0; attempt < 20; ++attempt) {
    const std::string key = "k" + std::to_string(attempt);
    c->Command({"SET", key, "v", "PX", "300"});
    c->Command({"WATCH", key});
    if (Pttl(*c, key) <= 0) {  // already stale at WATCH (loaded machine): EXEC would rightly succeed
      c->Command({"UNWATCH"});
      continue;
    }
    ASSERT_TRUE(WaitFor([&] { return Pttl(*c, key) <= 0; }));
    EXPECT_EQ(Transaction(*c, {{"PING"}}).type, RespValue::Type::kNull);
    return;
  }
  FAIL() << "the key was never live at WATCH";
}

TEST(MultiIT, ReaperDeletingAStaleWatchedKeyDoesNotAbort) {  // Review Focus #4
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  for (int attempt = 0; attempt < 20; ++attempt) {
    const std::string key = "k" + std::to_string(attempt);
    const int64_t baseline = InfoField(*c, "stats", "expired_keys");
    c->Command({"SET", key, "v", "PX", "50"});
    ASSERT_TRUE(WaitFor([&] { return Pttl(*c, key) <= 0; }));
    c->Command({"WATCH", key});                               // stale, not yet reaped, if we won the race
    if (InfoField(*c, "stats", "expired_keys") > baseline) {  // the reaper won: try again
      c->Command({"UNWATCH"});
      continue;
    }
    ASSERT_TRUE(WaitFor([&] { return InfoField(*c, "stats", "expired_keys") > baseline; }));
    EXPECT_EQ(Transaction(*c, {{"PING"}}).type, RespValue::Type::kArray);
    return;
  }
  FAIL() << "never observed WATCH on a stale key before the reaper deleted it";
}

// Proves a disconnected client's queued commands never run and that a fresh client's WATCH/EXEC on
// the same key works. It cannot observe a server-side watch-registry leak (no counter exists).
TEST(MultiIT, DisconnectedClientsQueueNeverRuns) {  // Review Focus #5
  TestServer s(TIDESKV_SERVER_BIN);
  for (int i = 0; i < 50; ++i) {
    auto c = s.Connect();
    ASSERT_NE(c, nullptr);
    c->Command({"WATCH", "k"});
    c->Command({"MULTI"});
    c->Command({"SET", "k", std::to_string(i)});
    c->Close();  // never EXECs
  }
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"GET", "k"}).type, RespValue::Type::kNull);
  c->Command({"WATCH", "k"});
  EXPECT_EQ(Transaction(*c, {{"SET", "k", "v"}}).type, RespValue::Type::kArray);
}

}  // namespace
}  // namespace tideskv::test
