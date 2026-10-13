#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "exec/engine.h"
#include "exec/gate.h"
#include "storage/memory_storage.h"
#include "support/command_harness.h"
#include "support/fake_clock.h"
#include "types/errors.h"

namespace tideskv {
namespace {

using namespace std::chrono_literals;

const std::string kBusy =
    test::Err("BUSY Redis is busy running a script. You can only call SCRIPT KILL or SHUTDOWN NOSAVE.");

TEST(BusyGate, WaitersGiveUpWhenWokenWithTheConditionTrue) {
  Gate g;
  g.lock();
  std::atomic<bool> busy{false};
  std::atomic<int> results{0};  // 1 = acquired, 2 = gave up
  std::thread reader([&] { results += g.lock_shared_unless([&] { return busy.load(); }) ? 1 : 2; });
  std::thread writer([&] { results += g.lock_unless([&] { return busy.load(); }) ? 1 : 2; });
  std::this_thread::sleep_for(30ms);
  EXPECT_EQ(results.load(), 0);  // both waiting
  busy = true;
  g.Wake();
  reader.join();
  writer.join();
  EXPECT_EQ(results.load(), 4);  // both gave up
  busy = false;
  g.unlock();
  EXPECT_TRUE(g.try_lock_shared());  // no stale writers_waiting_
  g.unlock_shared();
}

TEST(BusyGate, AFreeGateIsTakenEvenWhileTheConditionHolds) {
  Gate g;
  EXPECT_TRUE(g.lock_shared_unless([] { return true; }));  // give_up is only asked when waiting
  g.unlock_shared();
  EXPECT_TRUE(g.lock_unless([] { return true; }));
  g.unlock();
}

TEST(BusyGate, AWriterThatGivesUpLetsQueuedReadersIn) {
  Gate g;
  g.lock_shared();  // a reader holds the gate, so the writer waits and new readers queue behind it
  std::atomic<bool> busy{false};
  std::atomic<bool> writer_done{false};
  std::thread writer([&] {
    EXPECT_FALSE(g.lock_unless([&] { return busy.load(); }));
    writer_done = true;
  });
  while (g.try_lock_shared()) {  // wait until the writer is queued (writers_waiting_ > 0)
    g.unlock_shared();
    std::this_thread::yield();
  }
  std::atomic<bool> reader_in{false};
  std::thread reader([&] {
    g.lock_shared();  // plain lock_shared: must be released by the writer giving up
    reader_in = true;
    g.unlock_shared();
  });
  std::this_thread::sleep_for(30ms);
  EXPECT_FALSE(reader_in.load());
  busy = true;
  g.Wake();
  writer.join();
  reader.join();
  EXPECT_TRUE(writer_done.load());
  EXPECT_TRUE(reader_in.load());
  g.unlock_shared();
}

TEST(BusyGate, EngineCallsAnswerBusyWhileAScriptHoldsTheGate) {
  InMemoryStorage storage;
  LockManager locks;
  test::FakeClock clock;
  Engine engine(storage, locks, clock);
  std::atomic<bool> busy{false};
  engine.SetBusyGuard([&] { return busy.load(); }, "BUSY test");
  auto batch = engine.BeginBatch();
  ASSERT_TRUE(batch.ok());
  Status read, write;
  std::thread other([&] {
    read = engine.Read([](ReadView&, int64_t) { return Status::Ok(); });
    write = engine.Write(0, {"k"}, [](WriteTxn&, int64_t) { return Status::Ok(); });
  });
  std::this_thread::sleep_for(30ms);
  busy = true;
  engine.WakeWaiters();
  other.join();
  EXPECT_EQ(read.code(), StatusCode::kUserError);
  EXPECT_EQ(read.message(), "BUSY test");
  EXPECT_EQ(write.message(), "BUSY test");  // started after busy: gives up at once
  batch->reset();
}

TEST(BusyGate, EngineWithoutAGuardOrWithAFreeGateRunsNormally) {
  InMemoryStorage storage;
  LockManager locks;
  test::FakeClock clock;
  Engine engine(storage, locks, clock);
  EXPECT_TRUE(engine.Read([](ReadView&, int64_t) { return Status::Ok(); }).ok());
  engine.SetBusyGuard([] { return true; }, "BUSY test");
  EXPECT_TRUE(engine.Snapshot([](ReadView&, int64_t) { return Status::Ok(); }).ok());
  EXPECT_TRUE(engine.WriteExclusive([](WriteTxn&, int64_t) { return Status::Ok(); }).ok());
  auto batch = engine.BeginBatch();
  EXPECT_TRUE(batch.ok());
}

TEST(BusyDispatch, OnlyAllowBusyCommandsRunAndMultiIsFlagged) {  // redis72-lua.md §7-8
  test::CommandHarness h;
  h.services.scripts_.ForceTimedOutForTests(true);
  const std::string busy = kBusy;
  EXPECT_EQ(h.Run({"GET", "k"}), busy);
  EXPECT_EQ(h.Run({"PING"}), busy);
  EXPECT_EQ(h.Run({"SCRIPT", "LOAD", "return 1"}), busy);
  EXPECT_EQ(h.Run({"MULTI"}), test::kOk);
  EXPECT_EQ(h.Run({"SET", "k", "v"}), busy);
  EXPECT_EQ(h.Run({"EXEC"}),
            test::Err("EXECABORT Transaction discarded because of: BUSY Redis is busy running a "
                      "script. You can only call SCRIPT KILL or SHUTDOWN NOSAVE."));
  EXPECT_EQ(h.Run({"SHUTDOWN"}), busy);
  EXPECT_FALSE(h.services.shutdown_requested_);
  h.services.scripts_.ForceTimedOutForTests(false);
  EXPECT_EQ(h.Run({"GET", "k"}), test::kNil);
}

TEST(BusyDispatch, QueuedBusyCommandDoomsTheTransaction) {  // multi.tcl:539-640
  test::CommandHarness h;
  EXPECT_EQ(h.Run({"MULTI"}), test::kOk);
  h.services.scripts_.ForceTimedOutForTests(true);
  EXPECT_EQ(h.Run({"SET", "k", "v"}), kBusy);
  h.services.scripts_.ForceTimedOutForTests(false);
  EXPECT_EQ(h.Run({"EXEC"}), test::Err("EXECABORT Transaction discarded because of previous errors."));
  EXPECT_EQ(h.Run({"GET", "k"}), test::kNil);
}

TEST(BusyDispatch, AllowBusyCommandsStillRun) {
  test::CommandHarness h;
  h.services.scripts_.ForceTimedOutForTests(true);
  EXPECT_EQ(h.Run({"MULTI"}), test::kOk);
  EXPECT_EQ(h.Run({"DISCARD"}), test::kOk);
  EXPECT_EQ(h.Run({"UNWATCH"}), test::kOk);
  EXPECT_EQ(h.Run({"SHUTDOWN", "BADOPT"}), test::Err("ERR syntax error"));  // options parsed first
  EXPECT_FALSE(h.services.shutdown_requested_);
  h.Run({"SHUTDOWN", "NOSAVE"});
  EXPECT_TRUE(h.services.shutdown_requested_);
}

// End to end, as App wires it: a call already waiting for the gate when the script times out is
// woken by the timeout callback and answers BUSY; SCRIPT KILL then ends the script.
TEST(BusyDispatch, AWaitingCallIsWokenByTheTimeoutAndScriptKillEndsIt) {
  test::CommandHarness h;
  test::SecondClient other(h);
  ScriptEngine& scripts = h.services.scripts_;
  h.services.engine_.SetBusyGuard([&] { return scripts.TimedOut(); }, std::string(kErrBusyScript));
  scripts.SetOnTimeout([&] { h.services.engine_.WakeWaiters(); });
  ASSERT_EQ(h.Run({"CONFIG", "SET", "busy-reply-threshold", "300"}), test::kOk);
  std::string eval_reply;
  std::thread script([&] { eval_reply = h.Run({"EVAL", "while true do end", "0"}); });
  std::this_thread::sleep_for(100ms);  // the script holds the gate but has not timed out yet
  ASSERT_FALSE(scripts.TimedOut());
  std::atomic<bool> done{false};
  std::string get_reply;
  std::thread waiter([&] {
    get_reply = other.Run({"GET", "k"});  // passes Dispatch, then waits for the gate
    done = true;
  });
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!done.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
  if (!done.load()) scripts.KillForShutdown();  // lost wake-up: end the script so the test can fail
  waiter.join();
  EXPECT_TRUE(scripts.TimedOut());
  EXPECT_EQ(get_reply, kBusy);
  EXPECT_EQ(other.Run({"SCRIPT", "KILL"}), test::kOk);
  script.join();
  EXPECT_NE(eval_reply.find("Script killed by user with SCRIPT KILL..."), std::string::npos) << eval_reply;
  EXPECT_FALSE(scripts.TimedOut());
  EXPECT_EQ(other.Run({"GET", "k"}), test::kNil);
}

// Task 8 review: SCRIPT subcommands while a script is busy.
TEST(BusyDispatch, ScriptSubcommandsWhileBusy) {
  test::CommandHarness h;
  h.services.scripts_.ForceTimedOutForTests(true);
  EXPECT_EQ(h.Run({"SCRIPT", "LOAD", "return 1"}), kBusy);
  EXPECT_EQ(h.Run({"SCRIPT", "FLUSH"}), kBusy);
  EXPECT_EQ(h.Run({"SCRIPT", "EXISTS", "abc"}), kBusy);
  // KILL is never answered BUSY. The forced flag only makes TimedOut() true; no script actually
  // runs (phase is idle), so Kill() finds nothing to kill and the reply is NOTBUSY.
  EXPECT_EQ(h.Run({"SCRIPT", "KILL"}), test::Err("NOTBUSY No scripts in execution right now."));
  // Name and arity errors come before the busy check.
  EXPECT_EQ(h.Run({"SCRIPT", "FOO"}), test::Err("ERR unknown subcommand 'FOO'. Try SCRIPT HELP."));
  EXPECT_EQ(h.Run({"SCRIPT", "LOAD"}), test::Err("ERR wrong number of arguments for 'script|load' command"));
  EXPECT_EQ(h.Run({"SCRIPT", "KILL", "x"}),
            test::Err("ERR wrong number of arguments for 'script|kill' command"));
}

}  // namespace
}  // namespace tideskv
