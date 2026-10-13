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

constexpr char kBusyText[] =
    "BUSY Redis is busy running a script. You can only call SCRIPT KILL or SHUTDOWN NOSAVE.";
constexpr char kSpin[] = "local i = 0 while true do i = i + 1 end";

// Stops and joins the writer on every exit path, so a failed ASSERT cannot leave it joinable.
struct WriterGuard {
  std::atomic<bool>& stop;
  std::thread& thread;
  ~WriterGuard() {
    stop = true;
    if (thread.joinable()) thread.join();
  }
};

int64_t IntValue(RespClient& c, const std::string& key) {
  const RespValue v = c.Command({"GET", key});
  return v.type == RespValue::Type::kBulk ? std::stoll(v.str) : 0;
}

TEST(ScriptingIT, KillDuringEvalIsAllOrNothing) {  // Review Focus #1
  TempDir dir("tideskv-it-eval-kill");
  std::mt19937 rng(std::random_device{}());
  const std::string script = "for i = 1, 4000 do redis.call('incr','a') redis.call('incr','b') end return 1";
  for (int round = 0; round < 5; ++round) {
    {
      TestServer s(TIDESKV_SERVER_BIN, {.dir = dir.str()});
      auto c = s.Connect();
      ASSERT_NE(c, nullptr);
      ASSERT_TRUE(c->Send({"EVAL", script, "0"}));  // no reply awaited: we kill mid-script
      std::this_thread::sleep_for(std::chrono::milliseconds(10 + rng() % 200));
      s.Kill9();
    }
    TestServer s(TIDESKV_SERVER_BIN, {.dir = dir.str()});
    auto c = s.Connect();
    ASSERT_NE(c, nullptr);
    const int64_t a = IntValue(*c, "a");
    EXPECT_EQ(IntValue(*c, "b"), a) << "round " << round;
    EXPECT_EQ(a % 4000, 0) << "round " << round << ": a script is never partly applied";
  }
}

TEST(ScriptingIT, ReadersNeverSeeHalfAScript) {  // Review Focus #1
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--workers", "4"}});
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    auto c = s.Connect();
    while (c && !stop) c->Command({"EVAL", "redis.call('incr','a') redis.call('incr','b') return 1", "0"});
  });
  const WriterGuard guard{stop, writer};
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  for (int i = 0; i < 2000; ++i) {
    const RespValue r = c->Command({"MGET", "a", "b"});
    ASSERT_EQ(r.elements.size(), 2u);
    EXPECT_EQ(r.elements[0].str, r.elements[1].str);
  }
  stop = true;
  writer.join();  // the guard's destructor then sees it not joinable
  EXPECT_GT(IntValue(*c, "a"), 0);
}

TEST(ScriptingIT, BusyScriptAnswersBusyAndIsKilledWithItsWritesUndone) {  // Review Focus #3
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--workers", "2", "--lua-time-limit", "50"}});
  // Worker placement: workers run commands synchronously, so a connection on the runner's worker cannot
  // be served during the script. The server assigns accepted connections round-robin in accept order,
  // so with 2 workers the connection opened right after the runner is on the other worker.
  auto runner = s.Connect();
  auto other = s.Connect();
  ASSERT_TRUE(runner && other);
  ASSERT_TRUE(runner->Send({"EVAL", std::string("redis.call('set','w','1') ") + kSpin, "0"}));
  ASSERT_TRUE(WaitFor([&] { return other->Command({"GET", "k"}).str == kBusyText; }, 5s));
  EXPECT_EQ(other->Command({"PING"}).str, kBusyText);
  EXPECT_EQ(other->Command({"SCRIPT", "KILL"}).str, "OK");
  const auto killed = runner->Read();
  ASSERT_TRUE(killed.has_value());
  EXPECT_EQ(killed->type, RespValue::Type::kError);
  EXPECT_NE(killed->str.find("Script killed by user with SCRIPT KILL..."), std::string::npos) << killed->str;
  EXPECT_EQ(other->Command({"GET", "w"}).type, RespValue::Type::kNull);  // H6: rolled back
  EXPECT_EQ(runner->Command({"PING"}).str, "PONG");
  EXPECT_EQ(other->Command({"SCRIPT", "KILL"}).str, "NOTBUSY No scripts in execution right now.");
}

TEST(ScriptingIT, ClientsWaitingOnTheGateAreReleasedWithBusy) {  // Review Focus #3
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--workers", "2", "--lua-time-limit", "200"}});
  auto runner = s.Connect();
  ASSERT_NE(runner, nullptr);
  ASSERT_TRUE(runner->Send({"EVAL", kSpin, "0"}));
  std::this_thread::sleep_for(20ms);  // the script holds the gate, not yet timed out
  std::vector<std::unique_ptr<RespClient>> waiters;
  for (int i = 0; i < 8; ++i) {
    waiters.push_back(s.Connect());
    ASSERT_NE(waiters.back(), nullptr);
    ASSERT_TRUE(waiters.back()->Send({"GET", "k"}));  // these wait on the gate (both workers stall)
  }
  // Placement: connections go round-robin, so waiter i is on the runner's worker iff i is odd (the
  // runner is connection 0 of the sequence, waiter i is connection i + 1). Those waiters' commands are
  // not even read until the script ends. Even-indexed waiters are on the other worker: they must be
  // answered BUSY while the script still runs.
  for (size_t i = 0; i < waiters.size(); i += 2) {
    const auto r = waiters[i]->Read();  // RespClient times out after 5 s: a hang fails here
    ASSERT_TRUE(r.has_value()) << "waiter " << i;
    EXPECT_EQ(r->str, kBusyText) << "waiter " << i;
  }
  // 9 connections after the runner: the killer is on the other worker too (odd offset).
  auto killer = s.Connect();
  ASSERT_NE(killer, nullptr);
  EXPECT_EQ(killer->Command({"SCRIPT", "KILL"}).str, "OK");
  ASSERT_TRUE(runner->Read().has_value());
  // Waiters on the runner's worker run after the script ended: BUSY if already queued as busy, else the
  // normal reply (GET of a missing key is null).
  for (size_t i = 1; i < waiters.size(); i += 2) {
    const auto r = waiters[i]->Read();
    ASSERT_TRUE(r.has_value()) << "waiter " << i;
    EXPECT_TRUE(r->type == RespValue::Type::kNull || r->str == kBusyText) << ToDebugString(*r);
  }
}

TEST(ScriptingIT, ShutdownNosaveStopsABusyServer) {  // Review Focus #3
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--lua-time-limit", "50"}});
  // Round-robin placement (see the busy test above): `other` is on the other worker than `runner`.
  auto runner = s.Connect();
  auto other = s.Connect();
  ASSERT_TRUE(runner && other);
  ASSERT_TRUE(runner->Send({"EVAL", kSpin, "0"}));
  ASSERT_TRUE(WaitFor([&] { return other->Command({"GET", "k"}).str == kBusyText; }, 5s));
  EXPECT_EQ(other->Command({"SHUTDOWN"}).str, kBusyText);
  ASSERT_TRUE(other->Send({"SHUTDOWN", "NOSAVE"}));
  EXPECT_EQ(s.Wait(10s), 0);
}

TEST(ScriptingIT, ScriptCacheIsSharedByEveryWorker) {  // Review Focus #5
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--workers", "4"}});
  std::vector<std::unique_ptr<RespClient>> cs;
  for (int i = 0; i < 8; ++i) cs.push_back(s.Connect());
  const std::string sha = cs[0]->Command({"SCRIPT", "LOAD", "return 'hi'"}).str;
  for (auto& c : cs) EXPECT_EQ(c->Command({"EVALSHA", sha, "0"}).str, "hi");
  cs[3]->Command({"EVAL", "return 'eval'", "0"});
  const std::string sha2 = cs[5]->Command({"SCRIPT", "LOAD", "return 'eval'"}).str;
  for (auto& c : cs) EXPECT_EQ(c->Command({"SCRIPT", "EXISTS", sha2}).elements.at(0).integer, 1);
  EXPECT_EQ(InfoField(*cs[0], "memory", "number_of_cached_scripts"), 2);
  EXPECT_GT(InfoField(*cs[0], "memory", "used_memory_lua"), 0);
  cs[7]->Command({"SCRIPT", "FLUSH"});
  EXPECT_EQ(cs[0]->Command({"EVALSHA", sha, "0"}).str, "NOSCRIPT No matching script. Please use EVAL.");
}

TEST(ScriptingIT, ScriptWritesDirtyOtherClientsWatches) {  // Review Focus #5
  TestServer s(TIDESKV_SERVER_BIN);
  auto a = s.Connect();
  auto b = s.Connect();
  ASSERT_TRUE(a && b);
  b->Command({"WATCH", "w"});
  a->Command({"EVAL", "return redis.call('set','w','1')", "0"});
  b->Command({"MULTI"});
  EXPECT_EQ(b->Command({"EXEC"}).type, RespValue::Type::kNull);
  b->Command({"WATCH", "w"});
  a->Command({"EVAL", "return redis.call('set','w','1', 'NX')", "0"});  // no-op: not a change
  b->Command({"MULTI"});
  EXPECT_EQ(b->Command({"EXEC"}).type, RespValue::Type::kArray);
}

TEST(ScriptingIT, WaiterIsServedAfterTheScript) {  // Review Focus #5
  TestServer s(TIDESKV_SERVER_BIN);
  auto w = s.Connect();
  auto c = s.Connect();
  ASSERT_TRUE(w && c);
  ASSERT_TRUE(w->Send({"BLPOP", "q", "0"}));
  ASSERT_TRUE(WaitFor([&] { return InfoField(*c, "clients", "blocked_clients") == 1; }));
  EXPECT_EQ(c->Command({"EVAL", "redis.call('lpush','q','x') return redis.call('llen','q')", "0"}).integer,
            1);
  const auto got = w->Read();
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->elements.at(1).str, "x");
}

}  // namespace
}  // namespace tideskv::test
