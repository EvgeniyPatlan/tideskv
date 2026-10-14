#include <gtest/gtest.h>

#include "script/sha1hex.h"
#include "support/command_harness.h"

namespace tideskv::test {
namespace {

constexpr std::string_view kReadOnly = "READONLY You can't write against a read only replica.";

std::string Suffix(const std::string& code) { return " script: " + Sha1Hex(code) + ", on @user_script:1."; }

struct ReplicaHarness : CommandHarness {
  void BecomeReplica() { services.replication_.set_role(ReplRole::kReplica); }
};

TEST(ReadOnlyReplica, WritesAreRefusedReadsWork) {
  ReplicaHarness h;
  h.Run({"SET", "k", "v"});
  h.BecomeReplica();
  EXPECT_EQ(h.Run({"SET", "k", "w"}), Err(kReadOnly));
  EXPECT_EQ(h.Run({"FLUSHALL"}), Err(kReadOnly));
  EXPECT_EQ(h.Run({"GET", "k"}), Bulk("v"));
  EXPECT_EQ(h.Run({"PING"}), "+PONG\r\n");
  EXPECT_EQ(h.Run({"SELECT", "1"}), kOk);
  EXPECT_EQ(h.Run({"INFO", "server"}).rfind("$", 0), 0u);
  EXPECT_EQ(h.Run({"SCRIPT", "LOAD", "return 1"}), Bulk(Sha1Hex("return 1")));
}

TEST(ReadOnlyReplica, MultiRefusesAtQueueTime) {
  ReplicaHarness h;
  h.BecomeReplica();
  EXPECT_EQ(h.Run({"MULTI"}), kOk);
  EXPECT_EQ(h.Run({"SET", "k", "v"}), Err(kReadOnly));
  EXPECT_EQ(h.Run({"GET", "a"}), "+QUEUED\r\n");
  EXPECT_EQ(h.Run({"EXEC"}), Err("EXECABORT Transaction discarded because of previous errors."));
}

TEST(ReadOnlyReplica, ReadOnlyMultiExecWorks) {
  ReplicaHarness h;
  h.BecomeReplica();
  EXPECT_EQ(h.Run({"MULTI"}), kOk);
  EXPECT_EQ(h.Run({"GET", "a"}), "+QUEUED\r\n");
  EXPECT_EQ(h.Run({"EXEC"}), Arr({kNil}));
}

TEST(ReadOnlyReplica, RoleFlipBetweenQueueAndExecAbortsExec) {
  ReplicaHarness h;
  EXPECT_EQ(h.Run({"MULTI"}), kOk);
  EXPECT_EQ(h.Run({"SET", "k", "v"}), "+QUEUED\r\n");
  h.BecomeReplica();
  EXPECT_EQ(h.Run({"EXEC"}),
            Err("EXECABORT Transaction discarded because of: READONLY You can't write against a read only "
                "replica."));
  EXPECT_EQ(h.Run({"GET", "k"}), kNil);
  EXPECT_EQ(h.Run({"EXEC"}), Err("ERR EXEC without MULTI"));
}

TEST(ReadOnlyReplica, ScriptCallAndPcall) {
  ReplicaHarness h;
  h.BecomeReplica();
  const std::string call = "return redis.call('set','x','1')";
  EXPECT_EQ(h.Run({"EVAL", call, "0"}), Err(std::string(kReadOnly) + Suffix(call)));
  EXPECT_EQ(h.Run({"EVAL", "return redis.pcall('set','x','1')", "0"}), Err(kReadOnly));
  EXPECT_EQ(h.Run({"EVAL", "return redis.call('get','x')", "0"}), kNil);
  EXPECT_EQ(h.Run({"GET", "x"}), kNil);
}

TEST(ReadOnlyReplica, ShebangWithoutNoWritesIsRefused) {
  ReplicaHarness h;
  h.BecomeReplica();
  EXPECT_EQ(h.Run({"EVAL", "#!lua\nreturn 1", "0"}), Err(kReadOnly));
  EXPECT_EQ(h.Run({"EVAL", "#!lua flags=no-writes\nreturn 1", "0"}), Int(1));
}

TEST(ReadOnlyReplica, EvalRo) {
  ReplicaHarness h;
  h.BecomeReplica();
  const std::string c = "return redis.call('set','x','1')";
  EXPECT_EQ(h.Run({"EVAL_RO", c, "0"}),
            Err("ERR Write commands are not allowed from read-only scripts." + Suffix(c)));
  EXPECT_EQ(h.Run({"EVAL_RO", "#!lua\nreturn 1", "0"}), Err(kReadOnly));
}

TEST(ReadOnlyReplica, EvalRoShebangOnMasterKeepsItsOwnError) {
  ReplicaHarness h;
  EXPECT_EQ(h.Run({"EVAL_RO", "#!lua\nreturn 1", "0"}),
            Err("ERR Can not execute a script with write flag using *_ro command."));
}

TEST(ReadOnlyReplica, ReadOnlyComesBeforeBusy) {
  ReplicaHarness h;
  h.BecomeReplica();
  h.services.scripts().ForceTimedOutForTests(true);
  EXPECT_EQ(h.Run({"SET", "k", "v"}), Err(kReadOnly));
  EXPECT_EQ(h.Run({"EVAL", "#!lua\nreturn 1", "0"}), Err(kReadOnly));
  h.services.scripts().ForceTimedOutForTests(false);
}

TEST(ReadOnlyReplica, ShebangScriptCountsAsWriteInMulti) {
  ReplicaHarness h;
  h.BecomeReplica();
  EXPECT_EQ(h.Run({"MULTI"}), kOk);
  EXPECT_EQ(h.Run({"EVAL", "#!lua\nreturn 1", "0"}), Err(kReadOnly));
  EXPECT_EQ(h.Run({"EVAL", "#!lua flags=no-writes\nreturn 1", "0"}), "+QUEUED\r\n");
  EXPECT_EQ(h.Run({"EXEC"}), Err("EXECABORT Transaction discarded because of previous errors."));
}

TEST(ReadOnlyReplica, ShebangEvalshaUsesCachedFlags) {
  ReplicaHarness h;
  const std::string body = "#!lua\nreturn 1";
  const std::string sha = Sha1Hex(body);
  EXPECT_EQ(h.Run({"SCRIPT", "LOAD", body}), Bulk(sha));
  h.BecomeReplica();
  EXPECT_EQ(h.Run({"EVALSHA", sha, "0"}), Err(kReadOnly));
  EXPECT_EQ(h.Run({"EVALSHA", std::string(40, 'f'), "0"}),
            Err("NOSCRIPT No matching script. Please use EVAL."));
}

TEST(ReadOnlyReplica, PlainScriptInMultiFailsAtRunTime) {
  ReplicaHarness h;
  h.BecomeReplica();
  const std::string c = "return redis.call('set','x','1')";
  EXPECT_EQ(h.Run({"MULTI"}), kOk);
  EXPECT_EQ(h.Run({"EVAL", c, "0"}), "+QUEUED\r\n");
  EXPECT_EQ(h.Run({"EXEC"}), Arr({Err(std::string(kReadOnly) + Suffix(c))}));
}

}  // namespace
}  // namespace tideskv::test
