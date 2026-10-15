#include <gtest/gtest.h>

#include "commands/info.h"
#include "script/sha1hex.h"
#include "support/command_harness.h"

namespace tideskv::test {
namespace {

constexpr std::string_view kNoReplicas = "NOREPLICAS Not enough good replicas to write.";

TEST(NoReplicas, WritesAreRefusedReadsAndReadScriptsWork) {  // live redis:7.2.5
  CommandHarness h;
  h.Run({"SET", "a", "b"});
  h.services.replication_.good_replicas_ok = false;
  EXPECT_EQ(h.Run({"SET", "a", "c"}), Err(kNoReplicas));
  EXPECT_EQ(h.Run({"GET", "a"}), Bulk("b"));
  EXPECT_EQ(h.Run({"EVAL", "return redis.call('get','a')", "0"}), Bulk("b"));
  const std::string w = "return redis.call('set','a','b')";
  EXPECT_EQ(h.Run({"EVAL", w, "0"}),
            Err(std::string(kNoReplicas) + " script: " + Sha1Hex(w) + ", on @user_script:1."));
  EXPECT_EQ(h.Run({"EVAL", "#!lua\nreturn 1", "0"}), Err(kNoReplicas));  // a shebang write script: up front
  EXPECT_EQ(h.Run({"EVAL", "#!lua flags=no-writes\nreturn 1", "0"}), Int(1));
}

TEST(NoReplicas, MultiRefusesAtQueueTimeAndExecInheritsTheQueue) {
  CommandHarness h;
  h.services.replication_.good_replicas_ok = false;
  EXPECT_EQ(h.Run({"MULTI"}), kOk);
  EXPECT_EQ(h.Run({"SET", "a", "b"}), Err(kNoReplicas));
  EXPECT_EQ(h.Run({"EXEC"}), Err("EXECABORT Transaction discarded because of previous errors."));
  h.services.replication_.good_replicas_ok = true;
  EXPECT_EQ(h.Run({"MULTI"}), kOk);
  EXPECT_EQ(h.Run({"SET", "a", "b"}), "+QUEUED\r\n");
  h.services.replication_.good_replicas_ok = false;
  EXPECT_EQ(h.Run({"EXEC"}), Err("EXECABORT Transaction discarded because of: " + std::string(kNoReplicas)));
}

TEST(NoReplicas, CheckedBeforeReadOnly) {  // server.c:4057 before 4067
  CommandHarness h;
  h.services.replication_.good_replicas_ok = false;
  h.services.replication_.set_role(ReplRole::kReplica);
  EXPECT_EQ(h.Run({"SET", "a", "b"}),
            Err(kNoReplicas));  // the fake says so; the real manager never does on a replica
}

TEST(NoReplicas, InfoShowsGoodSlavesOnlyWhenEnabled) {
  ReplicationStats r;
  r.replid = std::string(40, 'a');
  EXPECT_EQ(FormatReplicationSection(r).find("min_slaves_good_slaves"), std::string::npos);
  r.min_replicas_enabled = true;
  r.good_replicas = 0;
  EXPECT_NE(FormatReplicationSection(r).find("connected_slaves:0\r\nmin_slaves_good_slaves:0\r\n"),
            std::string::npos);
}

}  // namespace
}  // namespace tideskv::test
