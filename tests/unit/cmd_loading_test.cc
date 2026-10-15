#include <gtest/gtest.h>

#include "commands/info.h"
#include "support/command_harness.h"

namespace tideskv::test {
namespace {

constexpr std::string_view kLoading = "LOADING Redis is loading the dataset in memory";

struct LoadingHarness : CommandHarness {
  LoadingHarness() {
    Run({"SET", "k", "v"});
    services.replication_.set_role(ReplRole::kReplica);
    services.repl_log_.FenceLocalWrites(true);
  }
  void StartLoad() { ASSERT_TRUE(services.engine_.BeginLoad({}).ok()); }
  void EndLoad() { ASSERT_TRUE(services.engine_.EndLoad(0, "", {}).ok()); }
};

TEST(Loading, CommandsWithoutTheLoadingFlagAreRefused) {  // server.c:4104
  LoadingHarness h;
  h.StartLoad();
  EXPECT_EQ(h.Run({"GET", "k"}), Err(kLoading));
  EXPECT_EQ(h.Run({"PING"}), Err(kLoading));
  EXPECT_EQ(h.Run({"DBSIZE"}), Err(kLoading));
  EXPECT_EQ(h.Run({"EVAL", "return 1", "0"}), Err(kLoading));
  EXPECT_EQ(h.Run({"SCRIPT", "LOAD", "return 1"}), Err(kLoading));  // only SCRIPT HELP is loading
}

TEST(Loading, LoadingCommandsWork) {
  LoadingHarness h;
  h.StartLoad();
  EXPECT_EQ(h.Run({"ECHO", "x"}), Bulk("x"));
  EXPECT_EQ(h.Run({"SELECT", "2"}), kOk);
  EXPECT_EQ(h.Run({"CONFIG", "GET", "maxclients"}).rfind("*2", 0), 0u);
  EXPECT_EQ(h.Run({"ROLE"}).rfind("*5", 0), 0u);
  EXPECT_EQ(h.Run({"INFO", "persistence"}).rfind("$", 0), 0u);
  EXPECT_EQ(h.Run({"REPLICAOF", "10.0.0.1", "7000"}), kOk);  // D7: repointing aborts a load
}

TEST(Loading, DebugWorksWhileLoading) {  // CF6
  LoadingHarness h;
  h.StartLoad();
  EXPECT_EQ(h.Run({"DEBUG", "DIGEST"}), "+" + std::string(40, '0') + "\r\n");  // the wiped keyspace
  EXPECT_EQ(h.Run({"DEBUG", "DIGEST-VALUE", "k"}), "*1\r\n+" + std::string(40, '0') + "\r\n");
  EXPECT_EQ(h.Run({"DEBUG", "HELP"}).rfind("*", 0), 0u);
}

TEST(Loading, NoOneIsRefusedWhileLoading) {  // D7: Redis's answer for this case
  LoadingHarness h;
  h.StartLoad();
  EXPECT_EQ(h.Run({"REPLICAOF", "NO", "ONE"}), Err(kLoading));
  h.EndLoad();
  EXPECT_EQ(h.Run({"REPLICAOF", "NO", "ONE"}), kOk);
}

TEST(Loading, MultiRefusesAtQueueTimeAndExecInheritsTheQueue) {
  LoadingHarness h;
  h.StartLoad();
  EXPECT_EQ(h.Run({"MULTI"}), kOk);
  EXPECT_EQ(h.Run({"GET", "k"}), Err(kLoading));
  EXPECT_EQ(h.Run({"EXEC"}), Err("EXECABORT Transaction discarded because of previous errors."));
  h.EndLoad();
  EXPECT_EQ(h.Run({"MULTI"}), kOk);
  EXPECT_EQ(h.Run({"GET", "k"}), "+QUEUED\r\n");
  h.StartLoad();
  EXPECT_EQ(h.Run({"EXEC"}), Err("EXECABORT Transaction discarded because of: " + std::string(kLoading)));
}

TEST(Loading, ExecOfAnEmptyOrAllLoadingQueueStillRuns) {  // PF7: only a non-loading queued command refuses
  LoadingHarness h;
  EXPECT_EQ(h.Run({"MULTI"}), kOk);
  EXPECT_EQ(h.Run({"ECHO", "x"}), "+QUEUED\r\n");
  h.StartLoad();
  EXPECT_EQ(h.Run({"EXEC"}), Arr({Bulk("x")}));
  h.EndLoad();
  EXPECT_EQ(h.Run({"MULTI"}), kOk);
  h.StartLoad();
  EXPECT_EQ(h.Run({"EXEC"}), "*0\r\n");
}

TEST(Loading, WatchAcrossAFullSyncFailsExec) {  // Review Focus #4: Redis invalidates every WATCH
  LoadingHarness h;
  EXPECT_EQ(h.Run({"WATCH", "nokey"}), kOk);
  h.StartLoad();
  h.EndLoad();
  EXPECT_EQ(h.Run({"MULTI"}), kOk);
  EXPECT_EQ(h.Run({"ECHO", "x"}), "+QUEUED\r\n");
  EXPECT_EQ(h.Run({"EXEC"}), "*-1\r\n");
}

TEST(Loading, WatchIsServedDuringALoad) {  // PF15/S11: a clean client reads through the LoadingReadScope
  LoadingHarness h;
  h.StartLoad();
  EXPECT_EQ(h.Run({"WATCH", "k"}), kOk);
}

TEST(Loading, PersistenceSectionShowsTheLoadingFields) {
  ServerStats s;
  s.loading = true;
  s.loading_start_time = 1700000000;
  s.loading_loaded_bytes = 4096;
  s.loading_eta_seconds = 0;
  const std::string info = FormatInfo(s, {"persistence"});
  EXPECT_EQ(info.rfind("# Persistence\r\nloading:1\r\nasync_loading:0\r\n", 0), 0u) << info;
  EXPECT_NE(
      info.find("\r\nloading_start_time:1700000000\r\nloading_total_bytes:0\r\nloading_rdb_used_mem:0\r\n"
                "loading_loaded_bytes:4096\r\nloading_loaded_perc:0.00\r\nloading_eta_seconds:0\r\n"),
      std::string::npos)
      << info;
  s.loading = false;
  const std::string idle = FormatInfo(s, {"persistence"});
  EXPECT_EQ(idle.rfind("# Persistence\r\nloading:0\r\nasync_loading:0\r\n", 0), 0u);
  EXPECT_EQ(idle.find("loading_start_time"), std::string::npos);
}

}  // namespace
}  // namespace tideskv::test
