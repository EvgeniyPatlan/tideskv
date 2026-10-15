#include <gtest/gtest.h>

#include "script/sha1hex.h"
#include "support/command_harness.h"

namespace tideskv::test {
namespace {

constexpr std::string_view kMasterDown =
    "MASTERDOWN Link with MASTER is down and replica-serve-stale-data is set to 'no'.";

struct StaleHarness : CommandHarness {
  StaleHarness() {
    Run({"SET", "k", "v"});
    services.replication_.set_role(ReplRole::kReplica);
    services.replication_.stale_refusing = true;
  }
};

TEST(StaleReplica, NonStaleCommandsGetMasterDown) {  // live redis:7.2.5
  StaleHarness h;
  EXPECT_EQ(h.Run({"GET", "k"}), Err(kMasterDown));
  EXPECT_EQ(h.Run({"PING"}), Err(kMasterDown));
  EXPECT_EQ(h.Run({"DBSIZE"}), Err(kMasterDown));
  EXPECT_EQ(h.Run({"SCRIPT", "EXISTS", "abc"}), Err(kMasterDown));
  EXPECT_EQ(h.Run({"EVAL", "return 1", "0"}), Err(kMasterDown));
  EXPECT_EQ(h.Run({"EVAL", "#!lua flags=no-writes\nreturn 1", "0"}), Err(kMasterDown));
}

TEST(StaleReplica, StaleCommandsWork) {
  StaleHarness h;
  EXPECT_EQ(h.Run({"ECHO", "hi"}), Bulk("hi"));
  EXPECT_EQ(h.Run({"SELECT", "0"}), kOk);
  EXPECT_EQ(h.Run({"INFO", "server"}).rfind("$", 0), 0u);
  EXPECT_EQ(h.Run({"CONFIG", "GET", "replica-serve-stale-data"}).rfind("*2", 0), 0u);
  EXPECT_EQ(h.Run({"SCRIPT", "LOAD", "return 1"}), Bulk(Sha1Hex("return 1")));
  EXPECT_EQ(h.Run({"EVAL", "#!lua flags=no-writes,allow-stale\nreturn 1", "0"}), Int(1));
}

TEST(StaleReplica, ReadOnlyIsCheckedBeforeMasterDown) {  // server.c:4067 before 4097
  StaleHarness h;
  EXPECT_EQ(h.Run({"SET", "k", "w"}), Err("READONLY You can't write against a read only replica."));
}

TEST(StaleReplica, AllowStaleScriptCallingANonStaleCommandFails) {  // script.c:504, live redis:7.2.5
  StaleHarness h;
  const std::string body = "#!lua flags=no-writes,allow-stale\nreturn redis.call('get','k')";
  EXPECT_EQ(h.Run({"EVAL", body, "0"}), Err("ERR Can not execute the command on a stale replica script: " +
                                            Sha1Hex(body) + ", on @user_script:2."));
}

TEST(StaleReplica, MultiRefusesAtQueueTimeAndExecInheritsTheQueue) {
  StaleHarness h;
  EXPECT_EQ(h.Run({"MULTI"}), kOk);
  EXPECT_EQ(h.Run({"GET", "k"}), Err(kMasterDown));
  EXPECT_EQ(h.Run({"EXEC"}), Err("EXECABORT Transaction discarded because of previous errors."));
  h.services.replication_.stale_refusing = false;  // the link goes down between queueing and EXEC
  EXPECT_EQ(h.Run({"MULTI"}), kOk);
  EXPECT_EQ(h.Run({"GET", "k"}), "+QUEUED\r\n");
  h.services.replication_.stale_refusing = true;
  EXPECT_EQ(h.Run({"EXEC"}), Err("EXECABORT Transaction discarded because of: " + std::string(kMasterDown)));
}

TEST(StaleReplica, NothingIsRefusedWhileServingStaleData) {
  StaleHarness h;
  h.services.replication_.stale_refusing = false;
  EXPECT_EQ(h.Run({"GET", "k"}), Bulk("v"));
}

}  // namespace
}  // namespace tideskv::test
