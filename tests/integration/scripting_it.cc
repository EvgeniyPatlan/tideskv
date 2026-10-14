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
  if (v.type == RespValue::Type::kNull) return 0;  // only a missing key counts as 0
  if (v.type != RespValue::Type::kBulk) {
    ADD_FAILURE() << "GET " << key << " answered " << ToDebugString(v);
    return -1;
  }
  return std::stoll(v.str);
}

// Task 13: batch writes are linear now, so the script can be long: about 0.6 s on a release server.
// Debug and sanitizer servers are 10x or more slower, so they run fewer iterations.
#ifdef NDEBUG
constexpr int kKillIterations = 300000;
#else
constexpr int kKillIterations = 30000;
#endif

TEST(ScriptingIT, EvalDeletingFortyCollectionsCommits) {  // D2: TidesDB's 32-range-per-commit limit
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  for (int i = 0; i < 40; ++i) ASSERT_EQ(c->Command({"HSET", "h" + std::to_string(i), "f", "v"}).integer, 1);
  EXPECT_EQ(c->Command({"EVAL", "for i=0,39 do redis.call('del','h'..i) end return 1", "0"}).integer, 1);
  EXPECT_EQ(c->Command({"DBSIZE"}).integer, 0);
}

TEST(ScriptingIT, KillDuringEvalIsAllOrNothing) {  // Review Focus #1
  TempDir dir("tideskv-it-eval-kill");
  std::mt19937 rng(std::random_device{}());
  const std::string script = "for i = 1, " + std::to_string(kKillIterations) +
                             " do redis.call('incr','a') redis.call('incr','b') end return 1";
  // One uninterrupted run times the script on this build. Kills then land anywhere from just after the
  // start to 1.5x its run time, so across rounds some land mid-script and some after its commit.
  int64_t run_ms = 0;
  {
    TestServer s(TIDESKV_SERVER_BIN, {.dir = dir.str()});
    auto c = RespClient::Connect(s.port(), 60s);
    ASSERT_NE(c, nullptr);
    const auto start = std::chrono::steady_clock::now();
    ASSERT_EQ(c->Command({"EVAL", script, "0"}).integer, 1);
    run_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                 .count();
    ASSERT_EQ(IntValue(*c, "a"), kKillIterations);
  }
  RecordProperty("script_ms", std::to_string(run_ms));
  for (int round = 0; round < 5; ++round) {
    const int64_t delay_ms = 10 + static_cast<int64_t>(rng() % static_cast<uint64_t>(run_ms * 3 / 2 + 1));
    {
      TestServer s(TIDESKV_SERVER_BIN, {.dir = dir.str()});
      auto c = s.Connect();
      ASSERT_NE(c, nullptr);
      ASSERT_TRUE(c->Send({"EVAL", script, "0"}));  // no reply awaited: we kill mid-script
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
      s.Kill9();
    }
    TestServer s(TIDESKV_SERVER_BIN, {.dir = dir.str()});
    auto c = s.Connect();
    ASSERT_NE(c, nullptr);
    const int64_t a = IntValue(*c, "a");
    SCOPED_TRACE("round " + std::to_string(round) + ", a = " + std::to_string(a));
    RecordProperty("round" + std::to_string(round),
                   std::to_string(delay_ms) + " ms: a = " + std::to_string(a));
    EXPECT_EQ(IntValue(*c, "b"), a) << "round " << round;
    EXPECT_EQ(a % kKillIterations, 0) << "round " << round << ": a script is never partly applied";
  }
}

// 50 incr pairs in one script: a non-atomic implementation would let a reader see a != b.
constexpr char kPairs[] = "for i = 1, 50 do redis.call('incr','a') redis.call('incr','b') end return 1";

TEST(ScriptingIT, ReadersNeverSeeHalfAScript) {  // Review Focus #1
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--workers", "4"}});
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    auto c = s.Connect();
    while (c && !stop) c->Command({"EVAL", kPairs, "0"});
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
  // Placement: connections are accepted round-robin, so connection n+1 is on the other worker than
  // connection n (2 workers). Order: runner (conn 0), probe (conn 1, other worker), waiter i (conn i + 2).
  // Waiter i therefore shares the runner's worker iff i is even; odd waiters and the probe do not.
  auto runner = s.Connect();
  auto probe = s.Connect();
  ASSERT_TRUE(runner && probe);
  ASSERT_TRUE(runner->Send({"EVAL", kSpin, "0"}));
  // Prove the script is inside the gate: a GET answered BUSY (after the 200 ms limit) can only come from
  // a running script; a null reply means the EVAL was not started yet, so ask again.
  ASSERT_TRUE(WaitFor([&] { return probe->Command({"GET", "k"}).str == kBusyText; }, 5s));
  std::vector<std::unique_ptr<RespClient>> waiters;
  for (int i = 0; i < 8; ++i) {
    waiters.push_back(s.Connect());
    ASSERT_NE(waiters.back(), nullptr);
    ASSERT_TRUE(waiters.back()->Send({"GET", "k"}));  // the gate is held: these must be answered BUSY
  }
  // Waiters on the runner's worker (even i) are not read by the server until the script ends. Odd ones are
  // on the other worker: they must be answered BUSY while the script still runs.
  for (size_t i = 1; i < waiters.size(); i += 2) {
    const auto r = waiters[i]->Read();  // RespClient times out after 5 s: a hang fails here
    ASSERT_TRUE(r.has_value()) << "waiter " << i;
    EXPECT_EQ(r->str, kBusyText) << "waiter " << i;
  }
  // The probe is on the other worker, so it can still be served: it is the killer.
  EXPECT_EQ(probe->Command({"SCRIPT", "KILL"}).str, "OK");
  const auto killed = runner->Read();
  ASSERT_TRUE(killed.has_value());
  EXPECT_NE(killed->str.find("Script killed by user with SCRIPT KILL..."), std::string::npos) << killed->str;
  // Waiters on the runner's worker run after the script ended: BUSY if already queued as busy, else the
  // normal reply (GET of a missing key is null).
  for (size_t i = 0; i < waiters.size(); i += 2) {
    const auto r = waiters[i]->Read();
    ASSERT_TRUE(r.has_value()) << "waiter " << i;
    EXPECT_TRUE(r->type == RespValue::Type::kNull || r->str == kBusyText) << ToDebugString(*r);
  }
}

TEST(ScriptingIT, ShutdownNosaveStopsABusyServer) {  // Review Focus #3
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--workers", "2", "--lua-time-limit", "50"}});
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
  for (int i = 0; i < 8; ++i) {
    cs.push_back(s.Connect());
    ASSERT_NE(cs.back(), nullptr);
  }
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
  EXPECT_EQ(b->Command({"WATCH", "w"}).str, "OK");
  EXPECT_EQ(a->Command({"EVAL", "return redis.call('set','w','1')", "0"}).str, "OK");
  b->Command({"MULTI"});
  EXPECT_EQ(b->Command({"EXEC"}).type, RespValue::Type::kNull);
  EXPECT_EQ(b->Command({"WATCH", "w"}).str, "OK");
  // no-op: not a change, so NX does not set and the reply is nil
  EXPECT_EQ(a->Command({"EVAL", "return redis.call('set','w','1', 'NX')", "0"}).type, RespValue::Type::kNull);
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

// Review fix A: a waiter whose key is pushed around a busy script is served once the script ends, and never
// receives -BUSY. Placement (3 workers, round-robin accept): waiter (conn 0, worker 0), runner (conn 1,
// worker 1), pusher (conn 2, worker 2), killer (conn 3, worker 0: free, as the waiter is parked). The
// pusher's LPUSH races the script: it lands before it (served at once), waits on the gate and answers BUSY,
// answers BUSY at dispatch, or commits just before the script takes the gate, so that its serve waits for the
// script (the pusher's worker is then stuck until the kill, hence a separate killer). If nothing was pushed,
// the pusher pushes again after the kill.
TEST(ScriptingIT, WaiterOfAKeyPushedAroundABusyScriptIsServedNotBusy) {
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--workers", "3", "--lua-time-limit", "50"}});
  auto waiter = s.Connect();
  auto runner = s.Connect();
  auto pusher = s.Connect();
  auto killer = s.Connect();
  ASSERT_TRUE(waiter && runner && pusher && killer);
  for (int round = 0; round < 5; ++round) {
    ASSERT_TRUE(waiter->Send({"BLPOP", "k", "0"}));
    ASSERT_TRUE(WaitFor([&] { return InfoField(*killer, "clients", "blocked_clients") == 1; }));
    ASSERT_TRUE(runner->Send({"EVAL", kSpin, "0"}));
    ASSERT_TRUE(pusher->Send({"LPUSH", "k", "v"}));
    ASSERT_TRUE(WaitFor([&] { return killer->Command({"SCRIPT", "KILL"}).str == "OK"; }, 5s));
    const auto killed = runner->Read();
    ASSERT_TRUE(killed.has_value());
    EXPECT_NE(killed->str.find("Script killed by user with SCRIPT KILL..."), std::string::npos)
        << killed->str;
    const auto pushed = pusher->Read();
    ASSERT_TRUE(pushed.has_value());
    ASSERT_TRUE(pushed->type == RespValue::Type::kInteger || pushed->str == kBusyText)
        << ToDebugString(*pushed);
    if (pushed->type != RespValue::Type::kInteger) {
      EXPECT_EQ(pusher->Command({"LPUSH", "k", "v"}).integer, 1);
    }
    const auto got = waiter->Read();  // RespClient times out after 5 s: a stranded waiter fails here
    ASSERT_TRUE(got.has_value()) << "round " << round;
    ASSERT_EQ(got->type, RespValue::Type::kArray) << ToDebugString(*got);
    EXPECT_EQ(got->elements.at(0).str, "k");
    EXPECT_EQ(got->elements.at(1).str, "v");
    EXPECT_EQ(killer->Command({"EXISTS", "k"}).integer, 0) << "round " << round;
  }
}

}  // namespace
}  // namespace tideskv::test
