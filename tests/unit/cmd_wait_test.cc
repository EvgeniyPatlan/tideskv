#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "support/command_harness.h"

namespace tideskv::test {
namespace {

TEST(WaitCmd, ParseErrorsAndImmediateAnswers) {  // live redis:7.2.5
  CommandHarness h;
  EXPECT_EQ(h.Run({"WAIT", "abc", "0"}), Err("ERR value is not an integer or out of range"));
  EXPECT_EQ(h.Run({"WAIT", "1", "abc"}), Err("ERR timeout is not an integer or out of range"));
  EXPECT_EQ(h.Run({"WAIT", "1", "1.5"}), Err("ERR timeout is not an integer or out of range"));
  EXPECT_EQ(h.Run({"WAIT", "1", "9223372036854775808"}),
            Err("ERR timeout is not an integer or out of range"));
  EXPECT_EQ(h.Run({"WAIT", "1", "-1"}), Err("ERR timeout is negative"));
  EXPECT_EQ(h.Run({"WAIT", "1", "9223372036854775807"}), Err("ERR timeout is out of range"));
  EXPECT_EQ(h.Run({"WAIT", "-5", "0"}), Int(0));
  EXPECT_EQ(h.Run({"WAIT", "0", "0"}), Int(0));
  EXPECT_EQ(h.Run({"WAIT"}), Err("ERR wrong number of arguments for 'wait' command"));
  EXPECT_EQ(h.session.blocked, nullptr);
}

TEST(WaitCmd, CountsOnlyAcksAtOrPastThisClientsWrite) {  // Review Focus #5
  CommandHarness h;
  h.services.replication_.acks = {0, 0};
  EXPECT_EQ(h.Run({"WAIT", "2", "100"}), Int(2));  // never wrote: woff 0
  EXPECT_EQ(h.Run({"SET", "k", "v"}), kOk);
  const uint64_t woff = h.session.woff;
  EXPECT_EQ(woff, h.services.repl_log_.last());
  EXPECT_GT(woff, 0u);
  h.services.replication_.acks = {woff, woff - 1};
  EXPECT_EQ(h.Run({"WAIT", "1", "100"}), Int(1));
  EXPECT_EQ(h.Run({"WAIT", "2", "100"}), "");  // parked
  ASSERT_EQ(h.services.replication_.waiters.size(), 1u);
  EXPECT_EQ(h.services.replication_.waiters[0].seq, woff);
  EXPECT_EQ(h.services.replication_.waiters[0].numreplicas, 2);
  EXPECT_NE(h.session.blocked, nullptr);
  EXPECT_EQ(h.services.replication_.waiters[0].client, h.session.blocked);
}

TEST(WaitCmd, AReadDoesNotMoveTheWriteOffset) {
  CommandHarness h;
  EXPECT_EQ(h.Run({"SET", "k", "v"}), kOk);
  const uint64_t woff = h.session.woff;
  SecondClient other(h);
  EXPECT_EQ(other.Run({"SET", "k2", "v"}), kOk);
  EXPECT_EQ(h.Run({"GET", "k2"}), Bulk("v"));
  EXPECT_EQ(h.session.woff, woff);  // another client's write is not this client's
  EXPECT_EQ(other.session.woff, h.services.repl_log_.last());
}

TEST(WaitCmd, ATransactionOrScriptThatWroteMovesTheWriteOffset) {
  CommandHarness h;
  EXPECT_EQ(h.Run({"MULTI"}), kOk);
  EXPECT_EQ(h.Run({"SET", "k", "v"}), "+QUEUED\r\n");
  EXPECT_EQ(h.Run({"EXEC"}), "*1\r\n" + kOk);
  EXPECT_EQ(h.session.woff, h.services.repl_log_.last());
  const uint64_t after_exec = h.session.woff;
  EXPECT_EQ(h.Run({"EVAL", "return redis.call('set','k2','v')", "0"}), kOk);
  EXPECT_GT(h.session.woff, after_exec);
  EXPECT_EQ(h.session.woff, h.services.repl_log_.last());
}

TEST(WaitCmd, NeverBlocksInsideMultiOrAScript) {  // live redis:7.2.5
  CommandHarness h;
  EXPECT_EQ(h.Run({"MULTI"}), kOk);
  EXPECT_EQ(h.Run({"WAIT", "1", "0"}), "+QUEUED\r\n");
  EXPECT_EQ(h.Run({"EXEC"}), "*1\r\n:0\r\n");
  EXPECT_EQ(h.Run({"EVAL", "return redis.call('wait','1','0')", "0"}), Int(0));
  EXPECT_EQ(h.session.blocked, nullptr);
}

TEST(WaitCmd, AScriptSeesItsCallersWriteOffset) {
  CommandHarness h;
  EXPECT_EQ(h.Run({"SET", "k", "v"}), kOk);
  h.services.replication_.acks = {h.session.woff - 1};
  EXPECT_EQ(h.Run({"EVAL", "return redis.call('wait','1','0')", "0"}), Int(0));
  h.services.replication_.acks = {h.session.woff};
  EXPECT_EQ(h.Run({"EVAL", "return redis.call('wait','1','0')", "0"}), Int(1));
}

TEST(WaitCmd, RefusedOnAReplicaBeforeParsing) {
  CommandHarness h;
  h.services.replication_.set_role(ReplRole::kReplica);
  const std::string err =
      "ERR WAIT cannot be used with replica instances. Please also note that since Redis 4.0 if a replica is "
      "configured to be writable (which is not the default) writes to replicas are just local and are not "
      "propagated.";
  EXPECT_EQ(h.Run({"WAIT", "abc", "x"}), Err(err));
  EXPECT_EQ(h.Run({"MULTI"}), kOk);
  EXPECT_EQ(h.Run({"WAIT", "1", "0"}), "+QUEUED\r\n");
  EXPECT_EQ(h.Run({"EXEC"}), "*1\r\n" + Err(err));
}

TEST(WaitCmd, TimeoutAndClientUnblockTimeoutReplyTheCount) {  // C11: CLIENT UNBLOCK covered too
  CommandHarness h;
  EXPECT_EQ(h.Run({"SET", "k", "v"}), kOk);
  h.services.replication_.acks = {h.session.woff};
  EXPECT_EQ(h.Run({"WAIT", "3", "0"}), "");
  ASSERT_NE(h.session.blocked, nullptr);
  EXPECT_EQ(h.session.blocked->TimeoutReply(), ":1\r\n");
  h.services.replication_.acks = {h.session.woff, h.session.woff};  // counted when the wait ends
  SecondClient admin(h);
  EXPECT_EQ(admin.Run({"CLIENT", "UNBLOCK", std::to_string(h.session.id), "TIMEOUT"}), Int(1));
  EXPECT_EQ(h.TakeUnblocked(), std::vector<std::string>{":2\r\n"});
}

TEST(WaitCmd, ClientUnblockTimeoutOfAListWaitStillRepliesNull) {
  CommandHarness h;
  EXPECT_EQ(h.Run({"BLPOP", "q", "0"}), "");
  ASSERT_NE(h.session.blocked, nullptr);
  EXPECT_EQ(h.session.blocked->TimeoutReply(), "");
  SecondClient admin(h);
  EXPECT_EQ(admin.Run({"CLIENT", "UNBLOCK", std::to_string(h.session.id), "TIMEOUT"}), Int(1));
  EXPECT_EQ(h.TakeUnblocked(), std::vector<std::string>{"*-1\r\n"});
}

TEST(WaitCmd, ServedBlockedCommandCarriesItsSeqToWait) {  // Review Focus #5
  CommandHarness h;
  EXPECT_EQ(h.Run({"BLPOP", "q", "0"}), "");  // parked
  auto waiter = h.session.blocked;
  ASSERT_NE(waiter, nullptr);
  EXPECT_FALSE(waiter->ended());
  SecondClient pusher(h);  // C11
  EXPECT_EQ(pusher.Run({"RPUSH", "q", "x"}), Int(1));
  const uint64_t push_seq = pusher.session.woff;
  EXPECT_TRUE(waiter->ended());
  EXPECT_EQ(waiter->served_seq(), h.services.repl_log_.last());  // the BLPOP's own pop, the newest commit
  EXPECT_GT(waiter->served_seq(), push_seq);
  EXPECT_EQ(pusher.session.woff, push_seq);  // serving the waiter is not the pusher's write
}

}  // namespace
}  // namespace tideskv::test
