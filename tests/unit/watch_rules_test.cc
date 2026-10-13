#include <gtest/gtest.h>

#include <cctype>

#include "server/watch.h"
#include "support/command_harness.h"

namespace tideskv::test {
namespace {

struct Rule {
  const char* name;
  std::vector<ArgList> setup;
  std::string watched;
  std::vector<ArgList> write;
  bool dirty;
};

const std::vector<Rule>& Rules() {
  static const std::vector<Rule> kRules = {
      // Not dirty: nothing changed (no signalModifiedKey).
      {"set nx on existing", {{"SET", "k", "v"}}, "k", {{"SET", "k", "w", "NX"}}, false},
      {"set xx on missing", {}, "k", {{"SET", "k", "w", "XX"}}, false},
      {"sadd present member", {{"SADD", "k", "a"}}, "k", {{"SADD", "k", "a"}}, false},
      {"srem absent member", {{"SADD", "k", "a"}}, "k", {{"SREM", "k", "zz"}}, false},
      {"del missing", {}, "k", {{"DEL", "k"}}, false},
      {"zadd same score", {{"ZADD", "k", "1", "a"}}, "k", {{"ZADD", "k", "1", "a"}}, false},
      {"hdel absent field", {{"HSET", "k", "f", "v"}}, "k", {{"HDEL", "k", "zz"}}, false},
      {"expire missing", {}, "k", {{"EXPIRE", "k", "100"}}, false},
      {"expire nx with ttl", {{"SET", "k", "v", "EX", "100"}}, "k", {{"EXPIRE", "k", "50", "NX"}}, false},
      {"persist without ttl", {{"SET", "k", "v"}}, "k", {{"PERSIST", "k"}}, false},
      {"lrem no match", {{"RPUSH", "k", "a"}}, "k", {{"LREM", "k", "0", "zz"}}, false},
      {"linsert no pivot", {{"RPUSH", "k", "a"}}, "k", {{"LINSERT", "k", "BEFORE", "zz", "x"}}, false},
      {"setrange empty", {{"SET", "k", "v"}}, "k", {{"SETRANGE", "k", "0", ""}}, false},
      {"rename same key", {{"SET", "k", "v"}}, "k", {{"RENAME", "k", "k"}}, false},
      {"renamenx failing", {{"SET", "k", "v"}, {"SET", "o", "v"}}, "k", {{"RENAMENX", "k", "o"}}, false},
      {"msetnx failing", {{"SET", "k", "v"}}, "k", {{"MSETNX", "k", "w", "o", "w"}}, false},
      {"hsetnx existing", {{"HSET", "k", "f", "v"}}, "k", {{"HSETNX", "k", "f", "w"}}, false},
      {"getex no option", {{"SET", "k", "v"}}, "k", {{"GETEX", "k"}}, false},
      {"smove absent member", {{"SADD", "k", "a"}}, "k", {{"SMOVE", "k", "d", "zz"}}, false},
      {"lpop missing", {}, "k", {{"LPOP", "k"}}, false},
      {"other key", {}, "k", {{"SET", "o", "v"}}, false},
      {"same name other db", {}, "k", {{"SELECT", "1"}, {"SET", "k", "v"}, {"SELECT", "0"}}, false},
      {"store source", {{"SADD", "s", "a"}}, "s", {{"SUNIONSTORE", "d", "s"}}, false},
      {"zstore source", {{"ZADD", "s", "1", "a"}}, "s", {{"ZUNIONSTORE", "d", "1", "s"}}, false},
      {"lmpop unpopped key",
       {{"RPUSH", "a", "x"}, {"RPUSH", "k", "y"}},
       "k",
       {{"LMPOP", "2", "a", "k", "LEFT"}},
       false},
      {"wrongtype error", {{"SET", "k", "v"}}, "k", {{"LPUSH", "k", "x"}}, false},
      {"flushdb missing key", {{"SET", "o", "v"}}, "k", {{"FLUSHDB"}}, false},
      {"flushdb other db", {{"SET", "k", "v"}}, "k", {{"SELECT", "1"}, {"FLUSHDB"}, {"SELECT", "0"}}, false},
      // Dirty: a real change, or a no-op Redis still signals.
      {"set same value", {{"SET", "k", "v"}}, "k", {{"SET", "k", "v"}}, true},
      {"hset same value", {{"HSET", "k", "f", "v"}}, "k", {{"HSET", "k", "f", "v"}}, true},
      {"ltrim no-op", {{"RPUSH", "k", "a"}}, "k", {{"LTRIM", "k", "0", "-1"}}, true},
      {"append empty", {{"SET", "k", "v"}}, "k", {{"APPEND", "k", ""}}, true},
      {"incrby zero", {{"SET", "k", "1"}}, "k", {{"INCRBY", "k", "0"}}, true},
      {"expire", {{"SET", "k", "v"}}, "k", {{"EXPIRE", "k", "100"}}, true},
      {"del", {{"SET", "k", "v"}}, "k", {{"DEL", "k"}}, true},
      {"create missing", {}, "k", {{"LPUSH", "k", "a"}}, true},
      {"rename source", {{"SET", "k", "v"}}, "k", {{"RENAME", "k", "o"}}, true},
      {"rename dest", {{"SET", "o", "v"}}, "k", {{"RENAME", "o", "k"}}, true},
      {"lmove dest", {{"RPUSH", "s", "a"}}, "k", {{"LMOVE", "s", "k", "LEFT", "LEFT"}}, true},
      {"smove dest already member",
       {{"SADD", "s", "a"}, {"SADD", "k", "a"}},
       "k",
       {{"SMOVE", "s", "k", "a"}},
       true},
      {"store dest", {{"SADD", "s", "a"}}, "k", {{"SUNIONSTORE", "k", "s"}}, true},
      {"zrangestore dest", {{"ZADD", "s", "1", "a"}}, "k", {{"ZRANGESTORE", "k", "s", "0", "-1"}}, true},
      {"lmpop popped key", {{"RPUSH", "k", "x"}}, "k", {{"LMPOP", "2", "k", "a", "LEFT"}}, true},
      {"zmpop popped key", {{"ZADD", "k", "1", "x"}}, "k", {{"ZMPOP", "1", "k", "MIN"}}, true},
      {"flushdb existing", {{"SET", "k", "v"}}, "k", {{"FLUSHDB"}}, true},
      {"flushall existing", {{"SET", "k", "v"}}, "k", {{"FLUSHALL"}}, true},
      {"getdel", {{"SET", "k", "v"}}, "k", {{"GETDEL", "k"}}, true},
  };
  return kRules;
}

class WatchRules : public ::testing::TestWithParam<size_t> {};

TEST_P(WatchRules, MatchRedis72) {  // Review Focus #3
  const Rule& r = Rules()[GetParam()];
  CommandHarness h;
  for (const ArgList& c : r.setup) h.Run(c);
  ASSERT_EQ(h.Run({"WATCH", r.watched}), kOk);
  for (const ArgList& c : r.write) h.Run(c);
  ASSERT_NE(h.session.watch, nullptr);
  EXPECT_EQ(h.session.watch->dirty(), r.dirty) << r.name;
}

INSTANTIATE_TEST_SUITE_P(Table, WatchRules, ::testing::Range<size_t>(0, Rules().size()), [](const auto& pi) {
  std::string n = Rules()[pi.param].name;
  for (char& ch : n) {
    if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
  }
  return n;
});

// The flag on a key that was already expired at WATCH time (multi.c:325, 393-402).
TEST(WatchExpiry, StaleKeyDeletionIsNotAChangeButRecreationIs) {  // Review Focus #4
  CommandHarness h;
  h.clock().Set(1000);
  h.Run({"SET", "k", "v", "PX", "100"});
  h.clock().Set(2000);
  ASSERT_EQ(h.Run({"WATCH", "k"}), kOk);
  EXPECT_EQ(h.Run({"DEL", "k"}), Int(0));  // lazily deletes the stale key
  EXPECT_FALSE(h.session.watch->dirty());
  h.Run({"SET", "k", "w"});
  EXPECT_TRUE(h.session.watch->dirty());
}

TEST(WatchExpiry, LazyDeletionOfALiveWatchedKeyByAMultiKeyCommandIsAChange) {
  CommandHarness h;
  h.clock().Set(1000);
  h.Run({"SADD", "k", "a"});
  h.Run({"PEXPIRE", "k", "100"});
  ASSERT_EQ(h.Run({"WATCH", "k"}), kOk);
  h.clock().Set(2000);
  h.Run({"SUNIONSTORE", "d", "k"});  // reads k for write: LoadForWrite deletes it (rule a)
  EXPECT_TRUE(h.session.watch->dirty());
}

TEST(WatchCommand, InsideMultiIsAnErrorThatDoesNotDoomTheTransaction) {
  CommandHarness h;
  h.session.multi.active = true;  // MULTI itself arrives in Task 5
  EXPECT_EQ(h.Run({"WATCH", "k"}), Err("ERR WATCH inside MULTI is not allowed"));
  EXPECT_FALSE(h.session.multi.dirty_exec);
}

TEST(WatchCommand, UnwatchClearsDirty) {
  CommandHarness h;
  h.Run({"WATCH", "k"});
  h.Run({"SET", "k", "v"});
  EXPECT_EQ(h.Run({"UNWATCH"}), kOk);
  EXPECT_FALSE(h.session.watch->dirty());
  EXPECT_TRUE(h.services.watches_.Idle());
}

TEST(WatchCommand, WatchOnADirtyClientRegistersNothing) {
  CommandHarness h;
  h.Run({"WATCH", "k"});
  h.Run({"SET", "k", "v"});
  EXPECT_EQ(h.Run({"WATCH", "x"}), kOk);
  EXPECT_EQ(h.session.watch->keys().size(), 1u);
}

TEST(WatchCommand, BareWatchIsAnArityError) {
  CommandHarness h;
  EXPECT_EQ(h.Run({"WATCH"}), Err("ERR wrong number of arguments for 'watch' command"));
}

}  // namespace
}  // namespace tideskv::test
