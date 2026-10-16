#include <gtest/gtest.h>

#include "support/command_harness.h"

namespace tideskv::test {
namespace {

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

}  // namespace
}  // namespace tideskv::test
