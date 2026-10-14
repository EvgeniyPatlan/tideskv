#include "script/script_engine.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "script/sha1hex.h"

namespace tideskv {
namespace {

class StubCaller final : public ScriptCaller {
 public:
  void SetReadOnly(bool ro) override { read_only = ro; }
  std::optional<std::string> Call(const std::vector<std::string>& argv, int, std::string*) override {
    calls.push_back(argv);
    return std::string(":1\r\n");
  }
  bool CommandExists(std::string_view) const override { return true; }
  bool read_only = false;
  std::vector<std::vector<std::string>> calls;
};

// Polls `pred` for up to 5 s so a regression fails instead of hanging the suite.
template <typename Pred>
bool WaitFor(Pred pred) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!pred()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::yield();
  }
  return true;
}

struct ScriptEngineFixture : ::testing::Test {
  ScriptEngine se;
  StubCaller caller;
  std::vector<std::string> keys, args;
  bool killed = false;

  std::string Eval(std::string_view script, bool by_sha = false, bool ro = false, int64_t threshold = 5000) {
    resp::ReplyBuilder out(2);
    killed = se.Eval({script, by_sha, ro, &keys, &args, threshold}, caller, out).killed;
    return out.Take();
  }
};

TEST_F(ScriptEngineFixture, EvalCachesAndEvalshaFindsItCaseInsensitively) {
  EXPECT_EQ(Eval("return 1+1"), ":2\r\n");
  EXPECT_FALSE(killed);
  EXPECT_EQ(se.cached_scripts(), 1u);
  EXPECT_EQ(Eval("a27e7e8a43702b7046d4f6a7ccf5b60cef6b9bd9", true), ":2\r\n");
  EXPECT_EQ(Eval("A27E7E8A43702B7046D4F6A7CCF5B60CEF6B9BD9", true), ":2\r\n");
  EXPECT_EQ(Eval("ffffffffffffffffffffffffffffffffffffffff", true),
            "-NOSCRIPT No matching script. Please use EVAL.\r\n");
  EXPECT_EQ(se.Exists({"a27e7e8a43702b7046d4f6a7ccf5b60cef6b9bd9", "A27E7E8A43702B7046D4F6A7CCF5B60CEF6B9BD9",
                       "nope"}),
            (std::vector<bool>{true, true, false}));
}

TEST_F(ScriptEngineFixture, LoadCompilesWithoutRunningAndRejectsBadScripts) {
  auto sha = se.Load("return 'loaded'");
  ASSERT_TRUE(sha.ok());
  EXPECT_EQ(*sha, "b534286061d4b9e4026607613b95c06c06015ae8");
  EXPECT_TRUE(caller.calls.empty());
  EXPECT_EQ(se.Load("return =").status().message().rfind("ERR Error compiling script (new function): ", 0),
            0u);
  EXPECT_EQ(se.Load("#!lua flags=bogus\nreturn 1").status().message(),
            "ERR Unexpected flag in script shebang: bogus");
  EXPECT_EQ(se.cached_scripts(), 1u);
}

TEST_F(ScriptEngineFixture, FlushEmptiesTheCacheAndTheState) {
  EXPECT_EQ(Eval("cjson.encode_max_depth(5) return 1"), ":1\r\n");
  se.Flush();
  EXPECT_EQ(se.cached_scripts(), 0u);
  EXPECT_EQ(Eval("a27e7e8a43702b7046d4f6a7ccf5b60cef6b9bd9", true),
            "-NOSCRIPT No matching script. Please use EVAL.\r\n");
  EXPECT_EQ(Eval("return cjson.encode_max_depth()"), ":1000\r\n");  // a fresh state
}

TEST_F(ScriptEngineFixture, ReadOnlyRules) {
  Eval("return 1", false, true);
  EXPECT_TRUE(caller.read_only);
  Eval("#!lua flags=no-writes\nreturn 1");
  EXPECT_TRUE(caller.read_only);
  Eval("return 1");
  EXPECT_FALSE(caller.read_only);
  EXPECT_EQ(Eval("#!lua\nreturn 1", false, true),
            "-ERR Can not execute a script with write flag using *_ro command.\r\n");
}

TEST_F(ScriptEngineFixture, TimesOutThenKillOrShutdownStopsIt) {
  std::atomic<bool> woke{false};
  se.SetOnTimeout([&] { woke = true; });
  EXPECT_FALSE(se.Kill());  // nothing running: NOTBUSY
  std::string reply;
  std::thread runner([&] { reply = Eval("while true do end", false, false, 20); });
  ASSERT_TRUE(WaitFor([&] { return se.TimedOut(); }));
  ASSERT_TRUE(WaitFor([&] { return woke.load(); }));  // the phase flips just before the callback runs
  EXPECT_TRUE(se.Kill());
  runner.join();
  EXPECT_TRUE(killed);
  EXPECT_NE(reply.find("Script killed by user with SCRIPT KILL..."), std::string::npos);
  EXPECT_FALSE(se.TimedOut());
  EXPECT_FALSE(se.Kill());  // the run is over: NOTBUSY, so a late SCRIPT KILL cannot hit the next script
  EXPECT_EQ(Eval("return 1"), ":1\r\n");  // the kill flag does not leak into the next run
  EXPECT_FALSE(killed);
}

TEST_F(ScriptEngineFixture, KillAfterATimedOutRunEndedDoesNotHitTheNextRun) {
  std::thread runner([&] { Eval("while true do end", false, false, 20); });
  ASSERT_TRUE(WaitFor([&] { return se.TimedOut(); }));
  EXPECT_TRUE(se.Kill());
  runner.join();
  EXPECT_FALSE(se.Kill());
  EXPECT_EQ(Eval("for i=1,300000 do end return 2", false, false, 1), ":2\r\n");
  EXPECT_FALSE(killed);
}

TEST_F(ScriptEngineFixture, ShutdownKillsAHookedScriptEvenBeforeItTimesOut) {
  std::string reply;
  std::thread runner([&] { reply = Eval("while true do end", false, false, 60000); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(se.TimedOut());
  se.KillForShutdown();
  runner.join();
  EXPECT_TRUE(killed);
  EXPECT_NE(reply.find("Script killed by user with SCRIPT KILL..."), std::string::npos);
}

// A caller whose redis.call answers SCRIPT KILL (as if it came from another worker) once the run timed out.
class KillingCaller final : public ScriptCaller {
 public:
  explicit KillingCaller(ScriptEngine& se) : se_(se) {}
  void SetReadOnly(bool) override {}
  std::optional<std::string> Call(const std::vector<std::string>&, int, std::string*) override {
    timed_out = se_.TimedOut();
    kill_answered_ok = se_.Kill();
    return std::string(":1\r\n");
  }
  bool CommandExists(std::string_view) const override { return true; }
  bool timed_out = false;
  bool kill_answered_ok = false;

 private:
  ScriptEngine& se_;
};

// B1: a SCRIPT KILL that lands after the script's last kill check (here: its last statement) but before the
// run ended either kills the run (its reply is the kill error, so its writes are undone) or answers NOTBUSY.
// It never answers +OK for a script that then completes normally.
TEST_F(ScriptEngineFixture, KillAnsweredOkAlwaysKillsTheRun) {
  KillingCaller killing(se);
  resp::ReplyBuilder out(2);
  const auto result = se.Eval({"local i = 0 while i < 3000000 do i = i + 1 end return redis.call('ping')",
                               false, false, &keys, &args, 1},
                              killing, out);
  const std::string reply = out.Take();
  ASSERT_TRUE(killing.timed_out) << "the loop ended before the 1 ms threshold";
  ASSERT_TRUE(killing.kill_answered_ok);
  EXPECT_TRUE(result.killed) << reply;
  EXPECT_EQ(reply.rfind("-ERR Script killed by user with SCRIPT KILL...", 0), 0u) << reply;
  EXPECT_FALSE(se.TimedOut());
  EXPECT_FALSE(se.Kill());  // the run is over
}

TEST_F(ScriptEngineFixture, LoadRefreshesTheLuaMemoryFigure) {  // B2: INFO used_memory_lua after SCRIPT LOAD
  const size_t before = se.lua_memory();
  std::string body = "local t = {";
  for (int i = 0; i < 2000; ++i) body += "'s" + std::to_string(i) + "',";
  body += "} return t";
  ASSERT_TRUE(se.Load(body).ok());
  EXPECT_GT(se.lua_memory(), before);
}

}  // namespace
}  // namespace tideskv
