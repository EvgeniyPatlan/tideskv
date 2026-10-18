#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "support/it_helpers.h"
#include "support/test_server.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

// Slot migration against real servers (M7b D25): two masters, A (0-8191) and B (8192-16383), driven as
// kvrocks-controller drives them. Node ids are 40 x 'a' and 40 x 'b'.
namespace tideskv::test {
namespace {

using namespace std::chrono_literals;

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define TIDESKV_IT_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define TIDESKV_IT_SANITIZED 1
#endif
#endif
#ifdef TIDESKV_IT_SANITIZED
constexpr auto kEraseBudget =
    1000ms;  // sanitizers slow the worker; the gocase's 100 ms runs in release (Task 15)
#else
constexpr auto kEraseBudget = 100ms;  // gocase "source server is killed while migrating"
#endif

const std::string kTryAgain = "TRYAGAIN Can't write to slot being migrated which is in write forbidden phase";

std::string Id(char c) { return std::string(40, c); }

std::string Line(char id, const TestServer& s, const std::string& slots) {
  return Id(id) + " 127.0.0.1 " + std::to_string(s.port()) + " master - " + slots + "\n";
}

void Push(const TestServer& s, char id, const std::string& nodes, int64_t version) {
  auto c = s.Connect();
  ASSERT_EQ(c->Command({"CLUSTERX", "SETNODEID", Id(id)}).str, "OK");
  ASSERT_EQ(c->Command({"CLUSTERX", "SETNODES", nodes, std::to_string(version)}).str, "OK");
}

struct Pair {
  Pair() {
    PushBoth(Line('a', a, "0-8191") + Line('b', b, "8192-16383"), 1);
    ac = a.Connect();
    bc = b.Connect();
  }
  void PushBoth(const std::string& nodes, int64_t version) {
    Push(a, 'a', nodes, version);
    Push(b, 'b', nodes, version);
  }
  std::string Moved(int slot) const {
    return "MOVED " + std::to_string(slot) + " 127.0.0.1:" + std::to_string(b.port());
  }

  TestServer a{TIDESKV_SERVER_BIN, {.extra_args = {"--cluster-enabled", "yes"}}};
  TestServer b{TIDESKV_SERVER_BIN, {.extra_args = {"--cluster-enabled", "yes"}}};
  std::unique_ptr<RespClient> ac;
  std::unique_ptr<RespClient> bc;
};

std::string Info(RespClient& c) { return c.Command({"CLUSTER", "INFO"}).str; }

bool WaitMigrate(RespClient& c, const std::string& range, const std::string& state,
                 std::chrono::milliseconds timeout = 30s) {
  return WaitFor(
      [&] {
        const std::string i = Info(c);
        return i.find("migrating_slot(s): " + range + "\r\n") != std::string::npos &&
               i.find("migrating_state: " + state + "\r\n") != std::string::npos;
      },
      timeout);
}

int64_t CountKeys(RespClient& c, int slot) {
  return c.Command({"CLUSTER", "COUNTKEYSINSLOT", std::to_string(slot)}).integer;
}

TEST(MigrationIT, MigrateRepliesBlocksAndMovedFollowKvrocks) {  // D1, D12, D15
  Pair p;
  ASSERT_EQ(p.ac->Command({"SET", "b", "1"}).str, "OK");
  ASSERT_EQ(p.ac->Command({"HSET", "{b}h", "f", "v"}).integer, 1);
  ASSERT_EQ(p.ac->Command({"SET", "{b}t", "v", "PX", "600000"}).str, "OK");
  EXPECT_EQ(p.ac->Command({"CLUSTERX", "MIGRATE", "3300", Id('z')}).str,
            "ERR Can't find the destination node id");
  EXPECT_EQ(p.ac->Command({"CLUSTERX", "MIGRATE", "12182", Id('b')}).str,
            "ERR Can't migrate slot which doesn't belong to me");
  EXPECT_EQ(p.ac->Command({"CLUSTERX", "MIGRATE", "0-5 10-20", Id('b')}).str,
            "ERR Can't migrate multiple slot ranges at a time");
  ASSERT_EQ(p.ac->Command({"CLUSTERX", "MIGRATE", "3300", Id('b')}).str, "OK");
  ASSERT_TRUE(WaitMigrate(*p.ac, "3300", "success"));
  EXPECT_NE(Info(*p.ac).find("destination_node: " + Id('b') + "\r\n"), std::string::npos);
  EXPECT_NE(Info(*p.bc).find("importing_slot(s): 3300\r\nimport_state: success\r\n"), std::string::npos);
  EXPECT_EQ(p.ac->Command({"GET", "b"}).str, p.Moved(3300));
  EXPECT_EQ(p.ac->Command({"DEL", "b"}).str, p.Moved(3300));
  EXPECT_EQ(p.bc->Command({"GET", "b"}).str, "1");  // served before any push (D1)
  EXPECT_EQ(p.bc->Command({"HGET", "{b}h", "f"}).str, "v");
  EXPECT_EQ(p.ac->Command({"DEBUG", "DIGEST-SLOTS", "3300", "TTL"}).str,
            p.bc->Command({"DEBUG", "DIGEST-SLOTS", "3300", "TTL"}).str);
  EXPECT_EQ(p.ac->Command({"CLUSTERX", "MIGRATE", "3300", Id('b')}).str,
            "ERR Can't migrate slot which has been migrated");
  EXPECT_EQ(CountKeys(*p.ac, 3300), 3) << "kept until a push moves the range (KM §5.5)";
}

TEST(MigrationIT, WritesDuringTheFenceAnswerTryagainAndEarlierOnesArrive) {  // D4, D25
  Pair p;
  ASSERT_EQ(p.ac->Command({"CONFIG", "SET", "migrate-delay-ms", "1000"}).str, "OK");
  ASSERT_EQ(p.ac->Command({"CLUSTERX", "MIGRATE", "3300", Id('b')}).str, "OK");
  int acked = 0;
  std::string refusal;
  ASSERT_TRUE(WaitFor(
      [&] {
        const RespValue r = p.ac->Command({"SET", "{b}" + std::to_string(acked), "v"});
        if (r.type != RespValue::Type::kError) {
          ++acked;
          return false;
        }
        refusal = r.str;
        return true;
      },
      20s));
  EXPECT_EQ(refusal, kTryAgain);
  EXPECT_GT(acked, 0);
  ASSERT_TRUE(WaitMigrate(*p.ac, "3300", "success"));
  for (int i = 0; i < acked; ++i) EXPECT_EQ(p.bc->Command({"GET", "{b}" + std::to_string(i)}).str, "v") << i;
  EXPECT_EQ(p.bc->Command({"EXISTS", "{b}" + std::to_string(acked)}).integer, 0)
      << "a refused write is nowhere";
}

TEST(MigrationIT, SyncRepliesOkOrNilAndTheJobGoesOn) {  // D12, KM §2.3
  Pair p;
  EXPECT_EQ(p.ac->Command({"CLUSTERX", "MIGRATE", "3300", Id('b'), "SYNC"}).str, "OK");
  EXPECT_NE(Info(*p.ac).find("migrating_state: success\r\n"), std::string::npos);
  ASSERT_EQ(p.ac->Command({"CONFIG", "SET", "migrate-delay-ms", "2000"}).str, "OK");
  const RespValue r = p.ac->Command({"CLUSTERX", "MIGRATE", "5061", Id('b'), "SYNC", "1"});
  EXPECT_EQ(r.type, RespValue::Type::kNull);
  EXPECT_NE(Info(*p.ac).find("migrating_state: start\r\n"), std::string::npos);
  EXPECT_TRUE(WaitMigrate(*p.ac, "5061", "success"));
}

TEST(MigrationIT, TheTargetErasesAHalfImportWhenTheSourceIsKilled) {  // D14, gocase: gone within 100 ms
  Pair p;
  ASSERT_EQ(p.ac->Command({"CONFIG", "SET", "migrate-batch-size-kb", "1"}).str, "OK");
  ASSERT_EQ(p.ac->Command({"CONFIG", "SET", "migrate-batch-rate-limit-mb", "1"}).str, "OK");
  const std::string value(512, 'a');
  for (int i = 0; i < 4000; ++i) ASSERT_EQ(p.ac->Command({"RPUSH", "{b}l", value}).integer, i + 1);  // ~2 s
  ASSERT_EQ(p.ac->Command({"CLUSTERX", "MIGRATE", "3300", Id('b')}).str, "OK");
  ASSERT_TRUE(WaitFor([&] { return CountKeys(*p.bc, 3300) == 1; }, 10s)) << "the meta arrives first";
  p.ac.reset();
  p.a.Kill9();
  EXPECT_TRUE(WaitFor([&] { return CountKeys(*p.bc, 3300) == 0; }, kEraseBudget));
  // The budget covers the erase only: the error record is written after it (C14), so wait for it.
  EXPECT_TRUE(WaitFor([&] { return Info(*p.bc).find("import_state: error\r\n") != std::string::npos; }, 10s));
}

TEST(MigrationIT, ARestartKeepsMigratedThenThePushCleansUp) {  // D8
  Pair p;
  ASSERT_EQ(p.ac->Command({"SET", "b", "1"}).str, "OK");
  ASSERT_EQ(p.ac->Command({"CLUSTERX", "MIGRATE", "3300", Id('b'), "SYNC"}).str, "OK");
  p.ac.reset();
  ASSERT_EQ(p.a.Terminate(), 0);
  p.a.Restart();
  p.ac = p.a.Connect();
  EXPECT_NE(Info(*p.ac).find("migrating_slot(s): 3300\r\ndestination_node: " + Id('b') +
                             "\r\nmigrating_state: success\r\n"),
            std::string::npos);
  EXPECT_EQ(p.ac->Command({"GET", "b"}).str, p.Moved(3300));
  EXPECT_EQ(p.ac->Command({"SET", "b", "2"}).str, p.Moved(3300));
  p.PushBoth(Line('a', p.a, "0-3299 3301-8191") + Line('b', p.b, "3300 8192-16383"), 2);
  EXPECT_EQ(CountKeys(*p.ac, 3300), 0);  // CLUSTERX waited for the cleanup (Writer's note 4)
  EXPECT_EQ(p.bc->Command({"GET", "b"}).str, "1");
  EXPECT_NE(Info(*p.ac).find("migrating_state: success\r\n"), std::string::npos);  // D15
}

TEST(MigrationIT, FlushslotsResetAndFlushFollowKvrocks) {  // D10, D13, D20
  Pair p;
  ASSERT_EQ(p.ac->Command({"CONFIG", "SET", "migrate-delay-ms", "60000"}).str, "OK");
  ASSERT_EQ(p.ac->Command({"CLUSTERX", "MIGRATE", "3300", Id('b')}).str, "OK");
  EXPECT_EQ(p.ac->Command({"CLUSTERX", "FLUSHSLOTS", "100"}).str,
            "ERR Cannot flush slot when migration is in progress");
  EXPECT_EQ(p.ac->Command({"CLUSTER", "RESET"}).str, "ERR Can't reset cluster while migrating slot");
  ASSERT_TRUE(WaitFor([&] { return Info(*p.bc).find("import_state: start") != std::string::npos; }, 10s));
  EXPECT_EQ(p.bc->Command({"CLUSTER", "RESET"}).str, "ERR Can't reset cluster while importing slot");
  ASSERT_EQ(p.ac->Command({"FLUSHALL"}).str, "OK");  // cancels the job (D10)
  ASSERT_TRUE(WaitMigrate(*p.ac, "3300", "fail"));
  ASSERT_TRUE(WaitFor([&] { return Info(*p.bc).find("import_state: error") != std::string::npos; }, 10s));
  EXPECT_EQ(p.ac->Command({"CLUSTER", "RESET", "HARD"}).str, "OK");
  EXPECT_EQ(p.ac->Command({"CLUSTERX", "VERSION"}).str, "-1");
}

}  // namespace
}  // namespace tideskv::test
