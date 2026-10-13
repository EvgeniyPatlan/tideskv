#include <gtest/gtest.h>

#include "commands/blocking_serve.h"
#include "support/command_harness.h"
#include "types/errors.h"

namespace tideskv {
namespace {

using test::Arr;
using test::Bulk;
using test::CommandHarness;
using test::Err;
using test::Int;
using test::kNil;
using test::kOk;
using test::SecondClient;
using Replies = std::vector<std::string>;

struct BlockingListCmds : ::testing::Test {
  // Runs argv on h with blocking denied (what MULTI/Lua will do in M5).
  std::string RunNoBlock(const ArgList& argv) {
    ExecContext ctx{h.session, h.reply, h.services};
    Dispatch(h.services.registry(), ctx, argv);
    return h.reply.Take();
  }
  CommandHarness h;
};

TEST_F(BlockingListCmds, DataPresentAnswersAtOnce) {
  EXPECT_EQ(h.Run({"RPUSH", "b", "1", "2"}), Int(2));
  EXPECT_EQ(h.Run({"BLPOP", "a", "b", "0"}), Arr({Bulk("b"), Bulk("1")}));
  EXPECT_EQ(h.Run({"BRPOP", "b", "0"}), Arr({Bulk("b"), Bulk("2")}));
  EXPECT_EQ(h.Run({"EXISTS", "b"}), Int(0));
  EXPECT_EQ(h.Run({"RPUSH", "c", "1", "2", "3"}), Int(3));
  EXPECT_EQ(h.Run({"BLMPOP", "0", "2", "a", "c", "RIGHT", "COUNT", "2"}),
            Arr({Bulk("c"), Arr({Bulk("3"), Bulk("2")})}));
  EXPECT_EQ(h.Run({"BLMOVE", "c", "d", "LEFT", "RIGHT", "0"}), Bulk("1"));
  EXPECT_EQ(h.Run({"RPUSH", "c", "x"}), Int(1));
  EXPECT_EQ(h.Run({"BRPOPLPUSH", "c", "d", "0"}), Bulk("x"));
  EXPECT_EQ(h.Run({"LRANGE", "d", "0", "-1"}), Arr({Bulk("x"), Bulk("1")}));
  EXPECT_EQ(h.services.blocking_.blocked_clients(), 0u);
}

TEST_F(BlockingListCmds, BlockThenServedByAPushFromAnotherClient) {  // Review Focus #2
  EXPECT_EQ(h.Run({"BLPOP", "k", "0"}), "");
  EXPECT_EQ(h.services.blocking_.blocked_clients(), 1u);
  SecondClient other(h);
  EXPECT_EQ(other.Run({"RPUSH", "k", "a", "b"}), Int(2));
  EXPECT_EQ(h.TakeUnblocked(), (Replies{Arr({Bulk("k"), Bulk("a")})}));
  EXPECT_EQ(other.Run({"LRANGE", "k", "0", "-1"}), Arr({Bulk("b")}));
}

TEST_F(BlockingListCmds, ServedKeyIsGoneBeforeThePusherRepliesAgain) {
  EXPECT_EQ(h.Run({"BRPOP", "k", "0"}), "");
  SecondClient other(h);
  EXPECT_EQ(other.Run({"LPUSH", "k", "only"}), Int(1));
  EXPECT_EQ(other.Run({"EXISTS", "k"}), Int(0));
  EXPECT_EQ(h.TakeUnblocked(), (Replies{Arr({Bulk("k"), Bulk("only")})}));
}

TEST_F(BlockingListCmds, WrongTypeIsImmediateAndScansInOrder) {  // Review Focus #3
  EXPECT_EQ(h.Run({"SET", "s", "v"}), kOk);
  EXPECT_EQ(h.Run({"BLPOP", "missing", "s", "0"}), Err(kErrWrongType));
  EXPECT_EQ(h.Run({"RPUSH", "l", "1"}), Int(1));
  EXPECT_EQ(h.Run({"BLPOP", "l", "s", "0"}), Arr({Bulk("l"), Bulk("1")}));  // a list before s wins
  EXPECT_EQ(h.Run({"BLMOVE", "s", "x", "LEFT", "LEFT", "0"}), Err(kErrWrongType));
  EXPECT_EQ(h.services.blocking_.blocked_clients(), 0u);
}

TEST_F(BlockingListCmds, ArgumentErrorsComeFirst) {
  EXPECT_EQ(h.Run({"SET", "s", "v"}), kOk);
  EXPECT_EQ(h.Run({"BLPOP", "s", "-1"}), Err(kErrTimeoutNegative));
  EXPECT_EQ(h.Run({"BLPOP", "s", "abc"}), Err(kErrTimeoutNotFloat));
  EXPECT_EQ(h.Run({"BLMPOP", "abc", "1", "s", "LEFT"}), Err(kErrTimeoutNotFloat));
  EXPECT_EQ(h.Run({"BLMPOP", "abc", "0", "s", "LEFT"}), Err(kErrNumkeysNotPositive));  // parse before timeout
  EXPECT_EQ(h.Run({"BLMOVE", "s", "x", "UP", "LEFT", "abc"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"BLMOVE", "s", "x", "LEFT", "LEFT", "abc"}), Err(kErrTimeoutNotFloat));
}

TEST_F(BlockingListCmds, NoBlockingContextAnswersAsTimedOut) {
  EXPECT_EQ(RunNoBlock({"BLPOP", "k", "0"}), "*-1\r\n");
  EXPECT_EQ(RunNoBlock({"BLMPOP", "0", "1", "k", "LEFT"}), "*-1\r\n");
  EXPECT_EQ(RunNoBlock({"BLMOVE", "k", "d", "LEFT", "LEFT", "0"}), kNil);  // null bulk, not array
  EXPECT_EQ(RunNoBlock({"BRPOPLPUSH", "k", "d", "0"}), kNil);
  EXPECT_EQ(h.services.blocking_.blocked_clients(), 0u);
}

TEST_F(BlockingListCmds, BlmoveWithWrongTypeDstErrorsWhenServed) {  // Review Focus #3
  EXPECT_EQ(h.Run({"SET", "dst", "v"}), kOk);
  EXPECT_EQ(h.Run({"BLMOVE", "src", "dst", "LEFT", "LEFT", "0"}), "");  // blocks: dst not checked yet
  SecondClient other(h);
  EXPECT_EQ(other.Run({"RPUSH", "src", "e"}), Int(1));
  EXPECT_EQ(h.TakeUnblocked(), (Replies{Err(kErrWrongType)}));
  EXPECT_EQ(other.Run({"LRANGE", "src", "0", "-1"}), Arr({Bulk("e")}));  // src intact
}

TEST_F(BlockingListCmds, BlmoveServingCascadesToTheDestinationsWaiters) {
  SecondClient b(h), pusher(h);
  b.session.id = 30;
  pusher.session.id = 31;
  EXPECT_EQ(h.Run({"BLMOVE", "src", "mid", "LEFT", "LEFT", "0"}), "");
  EXPECT_EQ(b.Run({"BLPOP", "mid", "0"}), "");
  EXPECT_EQ(pusher.Run({"RPUSH", "src", "e"}), Int(1));
  EXPECT_EQ(h.TakeUnblocked(), (Replies{Bulk("e")}));
  EXPECT_EQ(b.TakeUnblocked(), (Replies{Arr({Bulk("mid"), Bulk("e")})}));
  EXPECT_EQ(pusher.Run({"EXISTS", "src", "mid"}), Int(0));
}

TEST_F(BlockingListCmds, BlockedOnListStaysBlockedWhenKeyBecomesASet) {  // Review Focus #3
  EXPECT_EQ(h.Run({"BLPOP", "k", "0"}), "");
  SecondClient other(h);
  EXPECT_EQ(other.Run({"RENAME", "nokey", "k"}), Err(kErrNoSuchKey));
  EXPECT_EQ(other.Run({"SADD", "tmp", "m"}), Int(1));
  EXPECT_EQ(other.Run({"RENAME", "tmp", "k"}), kOk);  // signals k; a set does not serve a list waiter
  EXPECT_TRUE(h.TakeUnblocked().empty());
  EXPECT_EQ(h.services.blocking_.blocked_clients(), 1u);
}

TEST_F(BlockingListCmds, RenameIntoAWaitedKeyServes) {
  EXPECT_EQ(h.Run({"BLPOP", "k", "0"}), "");
  SecondClient other(h);
  EXPECT_EQ(other.Run({"RPUSH", "tmp", "v"}), Int(1));
  EXPECT_EQ(other.Run({"RENAME", "tmp", "k"}), kOk);
  EXPECT_EQ(h.TakeUnblocked(), (Replies{Arr({Bulk("k"), Bulk("v")})}));
}

TEST_F(BlockingListCmds, BlmpopServedTakesUpToCount) {
  EXPECT_EQ(h.Run({"BLMPOP", "0", "2", "a", "b", "LEFT", "COUNT", "5"}), "");
  SecondClient other(h);
  EXPECT_EQ(other.Run({"RPUSH", "b", "1", "2", "3"}), Int(3));
  EXPECT_EQ(h.TakeUnblocked(), (Replies{Arr({Bulk("b"), Arr({Bulk("1"), Bulk("2"), Bulk("3")})})}));
}

TEST_F(BlockingListCmds, DuplicateKeysAndSameKeyTwice) {
  EXPECT_EQ(h.Run({"BLPOP", "k", "k", "k", "0"}), "");
  EXPECT_EQ(h.services.blocking_.blocking_keys(), 1u);
  SecondClient other(h);
  EXPECT_EQ(other.Run({"LPUSH", "k", "v"}), Int(1));
  EXPECT_EQ(h.TakeUnblocked(), (Replies{Arr({Bulk("k"), Bulk("v")})}));
}

}  // namespace
}  // namespace tideskv
