#include <gtest/gtest.h>

#include <vector>

#include "support/command_harness.h"
#include "types/errors.h"

namespace tideskv {
namespace {

using test::Arr;
using test::Bulk;
using test::CommandHarness;
using test::Err;
using test::Int;
using test::kOk;
using test::SecondClient;
using Replies = std::vector<std::string>;

struct BlockingZsetCmds : ::testing::Test {
  CommandHarness h;
};

TEST_F(BlockingZsetCmds, DataPresent) {
  EXPECT_EQ(h.Run({"ZADD", "z", "1", "a", "2", "b"}), Int(2));
  EXPECT_EQ(h.Run({"BZPOPMIN", "nokey", "z", "0"}), Arr({Bulk("z"), Bulk("a"), Bulk("1")}));
  EXPECT_EQ(h.Run({"BZPOPMAX", "z", "0"}), Arr({Bulk("z"), Bulk("b"), Bulk("2")}));
  EXPECT_EQ(h.Run({"EXISTS", "z"}), Int(0));
  EXPECT_EQ(h.Run({"ZADD", "z", "1", "a", "2", "b"}), Int(2));
  EXPECT_EQ(h.Run({"BZMPOP", "0", "1", "z", "MAX", "COUNT", "5"}),
            Arr({Bulk("z"), Arr({Arr({Bulk("b"), Bulk("2")}), Arr({Bulk("a"), Bulk("1")})})}));
}

TEST_F(BlockingZsetCmds, ServedByZaddFromAnotherClient) {
  EXPECT_EQ(h.Run({"BZPOPMIN", "z", "0"}), "");
  SecondClient other(h);
  EXPECT_EQ(other.Run({"ZADD", "z", "5", "x", "3", "y"}), Int(2));
  EXPECT_EQ(h.TakeUnblocked(), (Replies{Arr({Bulk("z"), Bulk("y"), Bulk("3")})}));
  EXPECT_EQ(other.Run({"ZCARD", "z"}), Int(1));
}

TEST_F(BlockingZsetCmds, ServedByAStoreCreatingTheKey) {
  EXPECT_EQ(h.Run({"BZMPOP", "0", "1", "{t}dst", "MIN"}), "");
  SecondClient other(h);
  EXPECT_EQ(other.Run({"ZADD", "{t}src", "1", "m"}), Int(1));
  EXPECT_EQ(other.Run({"ZUNIONSTORE", "{t}dst", "1", "{t}src"}), Int(1));
  EXPECT_EQ(h.TakeUnblocked(), (Replies{Arr({Bulk("{t}dst"), Arr({Arr({Bulk("m"), Bulk("1")})})})}));
}

// Every *STORE that creates the destination wakes the client blocked on it.
TEST_F(BlockingZsetCmds, EachStoreCreatingTheKeyServes) {
  const std::vector<ArgList> stores = {
      {"ZUNIONSTORE", "{t}dst", "1", "{t}src"},
      {"ZINTERSTORE", "{t}dst", "1", "{t}src"},
      {"ZDIFFSTORE", "{t}dst", "1", "{t}src"},
      {"ZRANGESTORE", "{t}dst", "{t}src", "0", "-1"},
  };
  for (const ArgList& store : stores) {
    SCOPED_TRACE(store.front());
    EXPECT_EQ(h.Run({"BZPOPMIN", "{t}dst", "0"}), "");
    SecondClient other(h);
    EXPECT_EQ(other.Run({"ZADD", "{t}src", "1", "m"}), Int(1));
    EXPECT_EQ(other.Run(store), Int(1));
    EXPECT_EQ(h.TakeUnblocked(), (Replies{Arr({Bulk("{t}dst"), Bulk("m"), Bulk("1")})}));
    EXPECT_EQ(other.Run({"DEL", "{t}src"}), Int(1));
    EXPECT_EQ(other.Run({"EXISTS", "{t}dst"}), Int(0));
  }
}

TEST_F(BlockingZsetCmds, ListAndZsetWaitersOnOneKey) {  // Review Focus #3
  SecondClient list_waiter(h), pusher(h);
  list_waiter.session.id = 40;
  pusher.session.id = 41;
  EXPECT_EQ(list_waiter.Run({"BLPOP", "k", "0"}), "");
  EXPECT_EQ(h.Run({"BZPOPMIN", "k", "0"}), "");
  EXPECT_EQ(pusher.Run({"ZADD", "k", "1", "m"}), Int(1));
  EXPECT_TRUE(list_waiter.TakeUnblocked().empty());  // a zset never serves BLPOP
  EXPECT_EQ(h.TakeUnblocked(), (Replies{Arr({Bulk("k"), Bulk("m"), Bulk("1")})}));
  EXPECT_EQ(pusher.Run({"RPUSH", "k", "v"}), Int(1));  // k is gone; now a list
  EXPECT_EQ(list_waiter.TakeUnblocked(), (Replies{Arr({Bulk("k"), Bulk("v")})}));
}

TEST_F(BlockingZsetCmds, ErrorsAndNoBlockingContext) {
  EXPECT_EQ(h.Run({"SET", "s", "v"}), kOk);
  EXPECT_EQ(h.Run({"BZPOPMIN", "s", "0"}), Err(kErrWrongType));
  EXPECT_EQ(h.Run({"BZPOPMIN", "s", "-1"}), Err(kErrTimeoutNegative));
  EXPECT_EQ(h.Run({"BZMPOP", "x", "0", "s", "MIN"}), Err(kErrNumkeysNotPositive));  // mpop args first
  EXPECT_EQ(h.Run({"BZMPOP", "x", "1", "s", "MIN"}), Err(kErrTimeoutNotFloat));
  ExecContext ctx{h.session, h.reply, h.services};
  Dispatch(h.services.registry(), ctx, {"BZPOPMAX", "nokey", "0"});
  EXPECT_EQ(h.reply.Take(), "*-1\r\n");
  h.Resp3();
  Dispatch(h.services.registry(), ctx, {"BZMPOP", "0", "1", "nokey", "MIN"});
  EXPECT_EQ(h.reply.Take(), "_\r\n");
}

TEST_F(BlockingZsetCmds, Resp3ServedReplyIsFlatWithADouble) {
  h.Resp3();  // sets the session protocol too (Task 4), so the served reply is RESP3
  EXPECT_EQ(h.Run({"BZPOPMIN", "z", "0"}), "");
  SecondClient other(h);
  EXPECT_EQ(other.Run({"ZADD", "z", "1.5", "m"}), Int(1));
  EXPECT_EQ(h.TakeUnblocked(), (Replies{"*3\r\n" + Bulk("z") + Bulk("m") + ",1.5\r\n"}));
}

}  // namespace
}  // namespace tideskv
