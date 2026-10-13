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

TEST(MultiIT, ExecIsIsolatedFromConcurrentWriters) {  // Review Focus #2
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--workers", "4"}});
  std::atomic<bool> stop{false};
  std::vector<std::thread> writers;
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
  stop = true;
  for (auto& t : writers) t.join();
}

TEST(MultiIT, ReadersNeverSeeHalfAnExec) {  // Review Focus #2
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--workers", "4"}});
  std::atomic<bool> stop{false};
  std::thread writer([&] {
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
  writer.join();
}

TEST(MultiIT, WatchCasLoopIsExact) {  // Review Focus #1
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--workers", "4"}});
  constexpr int kClients = 8;
  constexpr int kIncrements = 50;
  std::atomic<int> errors{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kClients; ++t) {
    threads.emplace_back([&] {
      auto c = s.Connect();
      if (!c) return void(++errors);
      for (int done = 0; done < kIncrements;) {
        c->Command({"WATCH", "ctr"});
        const RespValue v = c->Command({"GET", "ctr"});
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
      std::thread client([&] {
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
      std::this_thread::sleep_for(std::chrono::milliseconds(50 + rng() % 200));
      s.Kill9();
      stop = true;
      client.join();
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
    ASSERT_TRUE(w->Read().has_value()) << "iteration " << i;  // the element or a null, never a hang
    pusher->Command({"DEL", "q"});
  }
}

TEST(MultiIT, WatchedKeyExpiringBeforeExecAborts) {  // Review Focus #4
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  c->Command({"SET", "k", "v", "PX", "100"});
  c->Command({"WATCH", "k"});
  std::this_thread::sleep_for(300ms);  // expired; the reaper may or may not have deleted it
  EXPECT_EQ(Transaction(*c, {{"PING"}}).type, RespValue::Type::kNull);
}

TEST(MultiIT, ReaperDeletingAStaleWatchedKeyDoesNotAbort) {  // Review Focus #4
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  c->Command({"SET", "k", "v", "PX", "50"});
  std::this_thread::sleep_for(100ms);
  c->Command({"WATCH", "k"});  // stale, or already reaped (then simply missing)
  ASSERT_TRUE(WaitFor([&] { return InfoField(*c, "stats", "expired_keys") >= 1; }));
  EXPECT_EQ(Transaction(*c, {{"PING"}}).type, RespValue::Type::kArray);
}

TEST(MultiIT, DisconnectInMultiLeavesNothingBehind) {  // Review Focus #5
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
