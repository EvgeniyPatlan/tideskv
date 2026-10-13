#include <gtest/gtest.h>

#include "support/command_harness.h"

namespace tideskv::test {
namespace {

const std::string kQueued = "+QUEUED\r\n";
const std::string kExecAbort = Err("EXECABORT Transaction discarded because of previous errors.");

TEST(Multi, ExecRunsQueuedCommandsInOrder) {
  CommandHarness h;
  EXPECT_EQ(h.Run({"MULTI"}), kOk);
  EXPECT_EQ(h.Run({"SET", "k", "v"}), kQueued);
  EXPECT_EQ(h.Run({"GET", "k"}), kQueued);
  EXPECT_EQ(h.Run({"INCR", "k"}), kQueued);
  EXPECT_EQ(h.Run({"EXEC"}), Arr({kOk, Bulk("v"), Err("ERR value is not an integer or out of range")}));
  EXPECT_FALSE(h.session.multi.active);
  EXPECT_EQ(h.Run({"GET", "k"}), Bulk("v"));
}

TEST(Multi, EmptyTransactionIsAnEmptyArray) {
  CommandHarness h;
  h.Run({"MULTI"});
  EXPECT_EQ(h.Run({"EXEC"}), "*0\r\n");
}

TEST(Multi, NothingIsVisibleBeforeExec) {
  CommandHarness h;
  SecondClient other(h);
  h.Run({"MULTI"});
  h.Run({"SET", "k", "v"});
  EXPECT_EQ(other.Run({"GET", "k"}), kNil);
  h.Run({"EXEC"});
  EXPECT_EQ(other.Run({"GET", "k"}), Bulk("v"));
}

TEST(Multi, DiscardDropsTheQueue) {
  CommandHarness h;
  h.Run({"MULTI"});
  h.Run({"SET", "k", "v"});
  EXPECT_EQ(h.Run({"DISCARD"}), kOk);
  EXPECT_EQ(h.Run({"GET", "k"}), kNil);
  EXPECT_EQ(h.Run({"EXEC"}), Err("ERR EXEC without MULTI"));
}

TEST(Multi, ErrorsThatDoNotDoomTheTransaction) {
  CommandHarness h;
  EXPECT_EQ(h.Run({"EXEC"}), Err("ERR EXEC without MULTI"));
  EXPECT_EQ(h.Run({"DISCARD"}), Err("ERR DISCARD without MULTI"));
  h.Run({"MULTI"});
  EXPECT_EQ(h.Run({"MULTI"}), Err("ERR MULTI calls can not be nested"));
  EXPECT_EQ(h.Run({"WATCH", "x"}), Err("ERR WATCH inside MULTI is not allowed"));
  h.Run({"SET", "k", "v"});
  EXPECT_EQ(h.Run({"EXEC"}), Arr({kOk}));
}

TEST(Multi, QueueTimeErrorsAbortExec) {  // multi.c:170, server.c:3708
  for (const ArgList& bad : std::vector<ArgList>{{"NOSUCH", "a"}, {"SET", "k"}, {"SHUTDOWN"}}) {
    CommandHarness h;
    h.Run({"MULTI"});
    h.Run({"SET", "a", "1"});
    EXPECT_EQ(h.Run(bad).substr(0, 5), "-ERR ") << bad[0];
    EXPECT_EQ(h.Run({"SET", "b", "2"}), kQueued);  // still +QUEUED, though dropped
    EXPECT_EQ(h.Run({"EXEC"}), kExecAbort);
    EXPECT_FALSE(h.session.multi.active);
    EXPECT_EQ(h.Run({"GET", "a"}), kNil);
    EXPECT_EQ(h.Run({"PING"}), "+PONG\r\n");  // out of MULTI again
  }
}

TEST(Multi, ShutdownIsNotAllowedInsideATransaction) {
  CommandHarness h;
  h.Run({"MULTI"});
  EXPECT_EQ(h.Run({"SHUTDOWN"}), Err("ERR Command not allowed inside a transaction"));
  EXPECT_FALSE(h.services.shutdown_requested_);
}

TEST(Multi, RejectedExecAbortsWithTheReason) {  // multi.c:136, even outside MULTI
  CommandHarness h;
  h.Run({"WATCH", "w"});
  EXPECT_EQ(h.Run({"EXEC", "x"}),
            Err("EXECABORT Transaction discarded because of: wrong number of arguments for 'exec' command"));
  EXPECT_TRUE(h.session.watch->keys().empty());  // discardTransaction unwatches
  h.Run({"MULTI"});
  h.Run({"SET", "k", "v"});
  EXPECT_EQ(h.Run({"EXEC", "x"}).substr(0, 10), "-EXECABORT");
  EXPECT_FALSE(h.session.multi.active);
  EXPECT_EQ(h.Run({"GET", "k"}), kNil);
}

TEST(Multi, UnauthenticatedExecAbortsWithNoauth) {
  CommandHarness h;
  h.RequirePassword("pw");
  EXPECT_EQ(h.Run({"EXEC"}),
            Err("EXECABORT Transaction discarded because of: NOAUTH Authentication required."));
}

TEST(Multi, RuntimeErrorsAreElementsAndNothingIsRolledBack) {
  CommandHarness h;
  h.Run({"SET", "s", "str"});
  h.Run({"MULTI"});
  h.Run({"SET", "a", "1"});
  h.Run({"LPUSH", "s", "x"});
  h.Run({"SET", "b", "2", "BADOPT"});
  h.Run({"SET", "c", "3"});
  EXPECT_EQ(h.Run({"EXEC"}),
            Arr({kOk, Err("WRONGTYPE Operation against a key holding the wrong kind of value"),
                 Err("ERR syntax error"), kOk}));
  EXPECT_EQ(h.Run({"MGET", "a", "b", "c"}), Arr({Bulk("1"), kNil, Bulk("3")}));
}

TEST(Multi, BlockingCommandsAnswerAsTimedOut) {  // multi.c DENY_BLOCKING; t_list.c:1268, 1292
  CommandHarness h;
  h.Run({"MULTI"});
  h.Run({"BLPOP", "q", "0"});
  h.Run({"BRPOP", "q", "0"});
  h.Run({"BLMOVE", "q", "d", "LEFT", "LEFT", "0"});
  h.Run({"BRPOPLPUSH", "q", "d", "0"});
  h.Run({"BLMPOP", "0", "1", "q", "LEFT"});
  h.Run({"BZPOPMIN", "z", "0"});
  h.Run({"BZMPOP", "0", "1", "z", "MIN"});
  EXPECT_EQ(h.Run({"EXEC"}), Arr({"*-1\r\n", "*-1\r\n", kNil, kNil, "*-1\r\n", "*-1\r\n", "*-1\r\n"}));
  EXPECT_EQ(h.services.blocking_.blocked_clients(), 0u);
}

TEST(Multi, BlockingCommandWithDataPops) {
  CommandHarness h;
  h.Run({"RPUSH", "q", "a"});
  h.Run({"MULTI"});
  h.Run({"BLPOP", "q", "0"});
  EXPECT_EQ(h.Run({"EXEC"}), Arr({Arr({Bulk("q"), Bulk("a")})}));
}

TEST(Multi, SelectInsideExecPersists) {
  CommandHarness h;
  h.Run({"MULTI"});
  h.Run({"SELECT", "3"});
  h.Run({"SET", "k", "v"});
  EXPECT_EQ(h.Run({"EXEC"}), Arr({kOk, kOk}));
  EXPECT_EQ(h.session.db, 3);
  EXPECT_EQ(h.Run({"GET", "k"}), Bulk("v"));
  h.Run({"SELECT", "0"});
  EXPECT_EQ(h.Run({"GET", "k"}), kNil);
}

TEST(Multi, HelloInsideExecSwitchesLaterElements) {  // multi.c header written first
  CommandHarness h;
  h.Run({"MULTI"});
  h.Run({"HELLO", "3"});
  h.Run({"GET", "missing"});
  const std::string r = h.Run({"EXEC"});
  EXPECT_EQ(r.substr(0, 4), "*2\r\n");
  EXPECT_EQ(r.substr(r.size() - 3), "_\r\n");  // the GET's null is RESP3
  EXPECT_EQ(h.session.protocol, 3);
  EXPECT_EQ(h.reply.protocol(), 3);
}

TEST(Multi, WaitersAreServedAfterTheWholeExec) {  // server.c:4169
  CommandHarness h;
  SecondClient w(h);
  EXPECT_EQ(w.Run({"BLPOP", "q", "0"}), "");  // blocked
  h.Run({"MULTI"});
  h.Run({"LPUSH", "q", "a"});
  h.Run({"LPOP", "q"});
  EXPECT_EQ(h.Run({"EXEC"}), Arr({Int(1), Bulk("a")}));
  EXPECT_TRUE(w.TakeUnblocked().empty());  // nothing left for it
  h.Run({"MULTI"});
  h.Run({"LPUSH", "q", "b"});
  h.Run({"LLEN", "q"});
  EXPECT_EQ(h.Run({"EXEC"}), Arr({Int(1), Int(1)}));  // still 1 inside EXEC
  EXPECT_EQ(w.TakeUnblocked(), (std::vector<std::string>{Arr({Bulk("q"), Bulk("b")})}));
}

TEST(Multi, WatchedKeyTouchedByAnotherClientMakesExecNull) {
  CommandHarness h;
  SecondClient other(h);
  h.Run({"WATCH", "k"});
  other.Run({"SET", "k", "x"});
  h.Run({"MULTI"});
  h.Run({"SET", "k", "mine"});
  EXPECT_EQ(h.Run({"EXEC"}), "*-1\r\n");
  EXPECT_EQ(h.Run({"GET", "k"}), Bulk("x"));
  EXPECT_TRUE(h.session.watch->keys().empty());
  EXPECT_EQ(h.Run({"EXEC"}), Err("ERR EXEC without MULTI"));
}

TEST(Multi, ExecAbortWinsOverCas) {
  CommandHarness h;
  SecondClient other(h);
  h.Run({"WATCH", "k"});
  other.Run({"SET", "k", "x"});
  h.Run({"MULTI"});
  h.Run({"NOSUCH"});
  EXPECT_EQ(h.Run({"EXEC"}), kExecAbort);
}

TEST(Multi, CasNullIsUnderscoreInResp3) {
  CommandHarness h;
  SecondClient other(h);
  h.Run({"HELLO", "3"});
  h.Run({"WATCH", "k"});
  other.Run({"SET", "k", "x"});
  h.Run({"MULTI"});
  EXPECT_EQ(h.Run({"EXEC"}), "_\r\n");
}

TEST(Multi, WritesInsideTheTransactionDoNotDirtyItsOwnWatch) {
  CommandHarness h;
  h.Run({"WATCH", "k"});
  h.Run({"MULTI"});
  h.Run({"SET", "k", "1"});
  EXPECT_EQ(h.Run({"EXEC"}), Arr({kOk}));
  h.Run({"MULTI"});
  h.Run({"SET", "k", "2"});
  EXPECT_EQ(h.Run({"EXEC"}), Arr({kOk}));  // unwatched by the previous EXEC
}

TEST(Multi, ExecWithoutMultiKeepsTheWatch) {  // Kvrocks multi #13
  CommandHarness h;
  SecondClient other(h);
  h.Run({"WATCH", "k"});
  other.Run({"SET", "k", "x"});
  EXPECT_EQ(h.Run({"EXEC"}), Err("ERR EXEC without MULTI"));
  h.Run({"MULTI"});
  EXPECT_EQ(h.Run({"EXEC"}), "*-1\r\n");
}

TEST(Multi, DiscardUnwatchesAndClearsDirty) {  // Kvrocks multi #26, #27
  CommandHarness h;
  h.Run({"SET", "x", "30"});
  h.Run({"WATCH", "x"});
  h.Run({"SET", "x", "40"});
  h.Run({"MULTI"});
  h.Run({"DISCARD"});
  h.Run({"MULTI"});
  h.Run({"INCR", "x"});
  EXPECT_EQ(h.Run({"EXEC"}), Arr({Int(41)}));
}

TEST(Multi, FlushallInsideExec) {
  CommandHarness h;
  h.Run({"SET", "a", "1"});
  h.Run({"MULTI"});
  h.Run({"FLUSHALL"});
  h.Run({"SET", "b", "2"});
  h.Run({"DBSIZE"});
  EXPECT_EQ(h.Run({"EXEC"}), Arr({kOk, kOk, Int(1)}));
  EXPECT_EQ(h.Run({"GET", "a"}), kNil);
}

TEST(Multi, ClockIsFrozenDuringExec) {
  CommandHarness h;
  h.clock().Set(1000);
  h.Run({"SET", "k", "v", "PX", "10"});
  h.Run({"MULTI"});
  h.Run({"GET", "k"});
  h.Run({"PTTL", "k"});
  EXPECT_EQ(h.Run({"EXEC"}), Arr({Bulk("v"), Int(10)}));
}

TEST(Multi, QuitInsideMultiRunsImmediately) {
  CommandHarness h;
  h.Run({"MULTI"});
  EXPECT_EQ(h.Run({"QUIT"}), kOk);
  EXPECT_TRUE(h.session.close_after_reply);
}

TEST(Multi, QueuedCommandsAreCopiedNotReparsed) {
  CommandHarness h;
  h.Run({"MULTI"});
  h.Run({"SET", std::string("k\0z", 3), std::string("v\r\n", 3)});
  h.Run({"GET", std::string("k\0z", 3)});
  EXPECT_EQ(h.Run({"EXEC"}), Arr({kOk, Bulk(std::string("v\r\n", 3))}));
}

}  // namespace
}  // namespace tideskv::test
