#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "support/command_harness.h"

namespace tideskv::test {
namespace {

using namespace std::chrono_literals;

// HOLDSET: a write that, once admitted, waits for the test before running SET k v (between Dispatch's
// pause check and the commit).
std::atomic<bool> g_hold_entered{false};
std::atomic<bool> g_hold_release{false};

void HoldSet(ExecContext& ctx, const ArgList&) {
  g_hold_entered = true;
  while (!g_hold_release) std::this_thread::sleep_for(1ms);
  ctx.server.registry().Find("set")->handler(ctx, {"SET", "k", "v"});
}

TEST(ClientPauseCmd, ParseErrorsAreRedis72s) {  // networking.c:285-303, timeout.c:165-196
  CommandHarness h;
  EXPECT_EQ(h.Run({"CLIENT", "PAUSE", "100", "SOMETIMES"}),
            Err("ERR CLIENT PAUSE mode must be WRITE or ALL"));
  EXPECT_EQ(h.Run({"CLIENT", "PAUSE", "abc"}), Err("ERR timeout is not an integer or out of range"));
  EXPECT_EQ(h.Run({"CLIENT", "PAUSE", "-1", "WRITE"}), Err("ERR timeout is negative"));
  EXPECT_EQ(h.Run({"CLIENT", "PAUSE", "100", "WRITE", "x"}),
            Err("ERR unknown subcommand or wrong number of arguments for 'PAUSE'. Try CLIENT HELP."));
  EXPECT_EQ(h.Run({"CLIENT", "UNPAUSE", "x"}),
            Err("ERR unknown subcommand or wrong number of arguments for 'UNPAUSE'. Try CLIENT HELP."));
  EXPECT_EQ(h.services.pause_.mode(), ClientPause::Mode::kOff);
}

TEST(ClientPauseCmd, AWriteIsPostponedNotRefusedAndRunsAfterUnpause) {
  CommandHarness h;
  SecondClient writer(h);
  ASSERT_EQ(h.Run({"CLIENT", "PAUSE", "60000", "WRITE"}), kOk);
  EXPECT_EQ(writer.Run({"SET", "k", "v"}), "");  // no reply yet: parked
  ASSERT_TRUE(writer.session.postponed.has_value());
  EXPECT_EQ(h.Run({"GET", "k"}), kNil);  // reads run; the write did not
  EXPECT_EQ(h.Run({"CLIENT", "UNPAUSE"}), kOk);
  EXPECT_EQ(writer.link->resumes(), 1);
  EXPECT_EQ(writer.RunPostponed(), kOk);  // what the connection does on Resume
  EXPECT_EQ(h.Run({"GET", "k"}), Bulk("v"));
}

TEST(ClientPauseCmd, WriteModeCoversEvalAndExecWithAQueuedWrite) {  // server.c:3914 may-replicate
  CommandHarness h;
  SecondClient c(h);
  ASSERT_EQ(h.Run({"CLIENT", "PAUSE", "60000", "WRITE"}), kOk);
  EXPECT_EQ(c.Run({"EVAL", "return 1", "0"}), "");
  c.session.postponed.reset();
  EXPECT_EQ(c.Run({"EVAL_RO", "return 1", "0"}), Int(1));  // not may-replicate
  ASSERT_EQ(h.Run({"CLIENT", "UNPAUSE"}), kOk);
  ASSERT_EQ(c.Run({"MULTI"}), kOk);
  ASSERT_EQ(c.Run({"GET", "k"}), "+QUEUED\r\n");
  ASSERT_EQ(h.Run({"CLIENT", "PAUSE", "60000", "WRITE"}), kOk);
  EXPECT_EQ(c.Run({"SET", "k", "v"}), "");  // a write is postponed even while queueing (server.c:4146)
  c.session.postponed.reset();
  ASSERT_EQ(h.Run({"CLIENT", "UNPAUSE"}), kOk);
  ASSERT_EQ(c.Run({"SET", "k", "v"}), "+QUEUED\r\n");
  ASSERT_EQ(h.Run({"CLIENT", "PAUSE", "60000", "WRITE"}), kOk);
  EXPECT_EQ(c.Run({"EXEC"}), "");
  ASSERT_EQ(h.Run({"CLIENT", "UNPAUSE"}), kOk);
  EXPECT_EQ(c.RunPostponed(), Arr({kNil, kOk}));
}

TEST(ClientPauseCmd, AllModePostponesEveryCommandIncludingUnpause) {  // tests/unit/pause.tcl:13-37
  CommandHarness h;
  SecondClient c(h);
  ASSERT_EQ(h.Run({"CLIENT", "PAUSE", "60000", "ALL"}), kOk);
  EXPECT_EQ(c.Run({"PING"}), "");
  c.session.postponed.reset();
  EXPECT_EQ(c.Run({"CLIENT", "UNPAUSE"}), "");
  h.services.pause_.Unpause();
  EXPECT_EQ(c.RunPostponed(), kOk);
}

// Review fix 1: CLIENT PAUSE replies only once every may-replicate command admitted before it committed
// (Redis runs them to completion before PAUSE can run).
TEST(ClientPauseCmd, PauseRepliesOnlyAfterAnInFlightWriteCommitted) {
  CommandHarness h;
  h.services.registry_.Register({"holdset", 1, kCmdWrite, 0, 0, 0, &HoldSet});
  SecondClient writer(h);
  SecondClient admin(h);
  g_hold_entered = false;
  g_hold_release = false;
  std::string write_reply;
  std::thread w([&] { write_reply = writer.Run({"HOLDSET"}); });
  while (!g_hold_entered) std::this_thread::sleep_for(1ms);
  std::atomic<bool> paused{false};
  std::string pause_reply;
  std::thread p([&] {
    pause_reply = admin.Run({"CLIENT", "PAUSE", "60000", "WRITE"});
    paused = true;
  });
  std::this_thread::sleep_for(100ms);
  EXPECT_FALSE(paused);  // the write is still in flight
  g_hold_release = true;
  p.join();
  w.join();
  EXPECT_EQ(pause_reply, kOk);
  EXPECT_EQ(write_reply, kOk);
  EXPECT_EQ(h.Run({"GET", "k"}), Bulk("v"));  // committed before the OK
  ASSERT_EQ(h.Run({"CLIENT", "UNPAUSE"}), kOk);
}

// Inside EXEC the barrier waits after the batch released the gate and leaves out the EXEC itself.
TEST(ClientPauseCmd, PauseInsideExecDoesNotWaitForItsOwnTransaction) {
  CommandHarness h;
  SecondClient other(h);
  ASSERT_EQ(h.Run({"MULTI"}), kOk);
  ASSERT_EQ(h.Run({"SET", "k", "v"}), "+QUEUED\r\n");
  ASSERT_EQ(h.Run({"CLIENT", "PAUSE", "60000", "WRITE"}), "+QUEUED\r\n");
  EXPECT_EQ(h.Run({"EXEC"}), Arr({kOk, kOk}));
  EXPECT_EQ(other.Run({"SET", "k", "w"}), "");  // the pause is in force
  ASSERT_EQ(h.Run({"CLIENT", "UNPAUSE"}), kOk);
  EXPECT_EQ(other.RunPostponed(), kOk);
}

}  // namespace
}  // namespace tideskv::test
