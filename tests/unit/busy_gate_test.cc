#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

// End to end, as App wires it: a script that loops forever, run by `h` on its own thread. Every exit
// path (including a failed ASSERT) ends the script and joins the thread.
class LoopingScript {
 public:
  static constexpr int kThresholdMs = 1000;

  explicit LoopingScript(test::CommandHarness& h) : h_(h), scripts_(h.services.scripts_) {
    h.services.engine_.SetBusyGuard([this] { return scripts_.TimedOut(); }, std::string(kErrBusyScript));
    scripts_.SetOnTimeout([this] { h_.services.engine_.WakeWaiters(); });
    EXPECT_EQ(h.Run({"CONFIG", "SET", "busy-reply-threshold", std::to_string(kThresholdMs)}), test::kOk);
    thread_ = std::thread([this] { reply_ = h_.Run({"EVAL", "while true do end", "0"}); });
  }
  ~LoopingScript() {
    if (!thread_.joinable()) return;
    scripts_.KillForShutdown();
    thread_.join();
  }
  LoopingScript(const LoopingScript&) = delete;
  LoopingScript& operator=(const LoopingScript&) = delete;

  // Runs each command on its own client and thread, all at once. A command still running after 10 s
  // means a lost wake-up: the script is ended so the waiters return and the test fails.
  std::vector<std::string> RunAll(std::vector<std::pair<test::SecondClient*, ArgList>> calls) {
    std::vector<std::string> replies(calls.size());
    std::atomic<size_t> done{0};
    std::vector<std::thread> threads;
    for (size_t i = 0; i < calls.size(); ++i) {
      threads.emplace_back([&, i] {
        replies[i] = calls[i].first->Run(calls[i].second);
        ++done;
      });
    }
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (done.load() < calls.size() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(5ms);
    if (done.load() < calls.size()) scripts_.KillForShutdown();
    for (std::thread& t : threads) t.join();
    return replies;
  }
  std::string Join() {
    thread_.join();
    return reply_;
  }

 private:
  test::CommandHarness& h_;
  ScriptEngine& scripts_;
  std::string reply_;
  std::thread thread_;
};

// A call already waiting for the gate when the script times out is woken by the timeout callback and
// answers BUSY; SCRIPT KILL then ends the script.
TEST(BusyDispatch, AWaitingCallIsWokenByTheTimeoutAndScriptKillEndsIt) {
  test::CommandHarness h;
  test::SecondClient other(h);
  LoopingScript script(h);
  std::this_thread::sleep_for(200ms);  // the script holds the gate but has not timed out yet
  ASSERT_FALSE(h.services.scripts_.TimedOut());
  const auto replies = script.RunAll({{&other, {"GET", "k"}}});  // passes Dispatch, waits for the gate
  EXPECT_TRUE(h.services.scripts_.TimedOut());
  EXPECT_EQ(replies[0], kBusy);
  EXPECT_EQ(other.Run({"SCRIPT", "KILL"}), test::kOk);
  const std::string eval_reply = script.Join();
  EXPECT_NE(eval_reply.find("Script killed by user with SCRIPT KILL..."), std::string::npos) << eval_reply;
  EXPECT_FALSE(h.services.scripts_.TimedOut());
  EXPECT_EQ(other.Run({"GET", "k"}), test::kNil);
}

// SCRIPT LOAD and EXISTS need ScriptEngine's mutex, which the script holds for its whole run. Sent
// before the timeout, they must wait on the gate (and be woken with BUSY), not on that mutex.
TEST(BusyDispatch, ScriptLoadAndExistsSentBeforeTheTimeoutAnswerBusy) {
  test::CommandHarness h;
  test::SecondClient loader(h);
  test::SecondClient checker(h);
  LoopingScript script(h);
  std::this_thread::sleep_for(200ms);
  ASSERT_FALSE(h.services.scripts_.TimedOut());
  const auto replies =
      script.RunAll({{&loader, {"SCRIPT", "LOAD", "return 1"}}, {&checker, {"SCRIPT", "EXISTS", "abc"}}});
  EXPECT_EQ(replies[0], kBusy);
  EXPECT_EQ(replies[1], kBusy);
  EXPECT_EQ(checker.Run({"SCRIPT", "KILL"}), test::kOk);
  script.Join();
  EXPECT_EQ(checker.Run({"SCRIPT", "EXISTS", "abc"}), test::Arr({test::Int(0)}));
}

// Review fix: a WATCH whose key read fails (BUSY: a timed-out script holds the gate) keeps none of the
// watches it registered.
TEST(BusyDispatch, WatchThatAnswersBusyLeavesNoWatches) {
  test::CommandHarness h;
  ScriptEngine& scripts = h.services.scripts_;
  h.services.engine_.SetBusyGuard([&] { return scripts.TimedOut(); }, std::string(kErrBusyScript));
  std::atomic<int> state{0};  // 0 = starting, 1 = holds the gate, 2 = BeginBatch failed
  std::atomic<bool> release{false};
  std::thread holder([&] {  // stands in for the script's batch
    auto batch = h.services.engine_.BeginBatch();
    state = batch.ok() ? 1 : 2;
    while (!release.load()) std::this_thread::sleep_for(1ms);
  });
  while (state.load() == 0) std::this_thread::sleep_for(1ms);
  scripts.ForceTimedOutForTests(true);
  const std::string reply = state.load() == 1 ? h.Run({"WATCH", "a", "b"}) : "no batch";
  const size_t registered = h.services.watches_.size();
  release = true;
  holder.join();  // before any ASSERT can return
  scripts.ForceTimedOutForTests(false);
  EXPECT_EQ(reply, kBusy);
  ASSERT_NE(h.session.watch, nullptr);
  EXPECT_TRUE(h.session.watch->keys().empty());
  EXPECT_EQ(registered, 0u);
  EXPECT_EQ(h.Run({"WATCH", "a"}), test::kOk);
  EXPECT_EQ(h.services.watches_.size(), 1u);
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
