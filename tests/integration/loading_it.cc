#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "support/full_sync.h"
#include "support/it_helpers.h"
#include "support/test_server.h"

namespace tideskv::test {
namespace {

using namespace std::chrono_literals;

constexpr const char* kLoadingText = "LOADING Redis is loading the dataset in memory";

// key-load-delay (D14) stretches the load so that clients meet it.
TEST(LoadingIT, AReplicaLoadingAFullSyncAnswersLoadingThenServesTheData) {
  TestServer master(TIDESKV_SERVER_BIN);
  TestServer replica(TIDESKV_SERVER_BIN);
  auto m = master.Connect();
  ASSERT_TRUE(FillKeys(*m, 5000));
  auto r = replica.Connect();
  ASSERT_TRUE(ForceFullSync(*r));
  ASSERT_EQ(r->Command({"CONFIG", "SET", "key-load-delay", "400"}).str, "OK");  // ~2 s for 5000 keys
  ASSERT_EQ(r->Command({"REPLICAOF", "127.0.0.1", std::to_string(master.port())}).str, "OK");
  ASSERT_TRUE(WaitFor([&] { return InfoField(*r, "persistence", "loading") == 1; }, 10s));
  const RespValue get = r->Command({"GET", "k1"});
  EXPECT_EQ(get.type, RespValue::Type::kError);
  EXPECT_EQ(get.str, kLoadingText);
  EXPECT_EQ(r->Command({"DEBUG", "DIGEST"}).type, RespValue::Type::kSimple);
  EXPECT_GT(InfoField(*r, "persistence", "loading_start_time"), 0);
  ASSERT_TRUE(WaitFor([&] { return InfoField(*r, "persistence", "loading") == 0; }, 60s));
  ASSERT_EQ(r->Command({"CONFIG", "SET", "key-load-delay", "0"}).str, "OK");
  EXPECT_EQ(r->Command({"GET", "k4999"}).str, "v");
  EXPECT_EQ(r->Command({"DEBUG", "DIGEST"}).str, m->Command({"DEBUG", "DIGEST"}).str);
}

// PF15/S11: a clean watching client is invalidated by the load (EXEC nil); a fresh one can WATCH during it.
TEST(LoadingIT, WatchAcrossAFullSyncFailsExecAndWatchIsServedDuringTheLoad) {
  TestServer master(TIDESKV_SERVER_BIN);
  TestServer replica(TIDESKV_SERVER_BIN);
  auto m = master.Connect();
  ASSERT_TRUE(FillKeys(*m, 3000));
  auto r = replica.Connect();
  ASSERT_TRUE(ForceFullSync(*r));
  auto watcher = replica.Connect();
  ASSERT_EQ(watcher->Command({"WATCH", "nokey"}).str, "OK");  // clean: nothing has dirtied it yet
  ASSERT_EQ(r->Command({"CONFIG", "SET", "key-load-delay", "400"}).str, "OK");
  ASSERT_EQ(r->Command({"REPLICAOF", "127.0.0.1", std::to_string(master.port())}).str, "OK");
  ASSERT_TRUE(WaitFor([&] { return InfoField(*r, "persistence", "loading") == 1; }, 10s));
  auto fresh = replica.Connect();
  EXPECT_EQ(fresh->Command({"WATCH", "k1"}).str, "OK");  // WATCH carries the loading flag
  EXPECT_EQ(fresh->Command({"GET", "k1"}).str, kLoadingText);
  ASSERT_TRUE(WaitFor([&] { return InfoField(*r, "persistence", "loading") == 0; }, 60s));
  ASSERT_EQ(watcher->Command({"MULTI"}).str, "OK");
  ASSERT_EQ(watcher->Command({"PING"}).str, "QUEUED");
  EXPECT_EQ(watcher->Command({"EXEC"}).type, RespValue::Type::kNull);
}

}  // namespace
}  // namespace tideskv::test
