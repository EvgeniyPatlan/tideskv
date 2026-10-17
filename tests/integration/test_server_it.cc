#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "support/test_server.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace tideskv::test {
namespace {

uint16_t g_taken_port = 0;  // EXPECT_NONFATAL_FAILURE's statement may not use locals
std::string g_fake_server;  // likewise
std::string g_marker;       // likewise
std::string g_data_dir;     // likewise

// A stand-in for tideskv-server: prints `lines` to stderr, then idles until SIGTERM and exits 0, so only
// the log can make the harness fail the test.
std::string WriteFakeServer(const TempDir& dir, const std::string& lines) {
  const auto path = dir.path() / "fake-server.sh";
  std::ofstream(path) << "#!/bin/sh\n"
                      << "trap 'exit 0' TERM\n"
                      << lines << "\n"
                      << "while :; do sleep 0.05; done\n";
  std::filesystem::permissions(path, std::filesystem::perms::owner_all);
  return path.string();
}

bool WaitForLog(const TestServer& s, const std::string& needle) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    if (s.Stderr().find(needle) != std::string::npos) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

// A child that cannot bind its port must never be reported ready because some other server
// (here: `first`) answers PING on that port (M5a plan G6).
TEST(TestServerIT, ForeignServerOnThePortIsNotReady) {
  TestServer first(TIDESKV_SERVER_BIN);
  g_taken_port = first.port();
  EXPECT_NONFATAL_FAILURE(
      { TestServer second(TIDESKV_SERVER_BIN, {.port = g_taken_port}); }, "did not become ready");
  auto c = first.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"PING"}).str, "PONG");
}

// Many servers started at once from many threads all come up, each on its own port.
TEST(TestServerIT, ParallelStartsGetDistinctPorts) {
  constexpr int kServers = 12;
  std::vector<std::unique_ptr<TestServer>> servers(kServers);
  std::vector<std::thread> threads;
  for (int i = 0; i < kServers; ++i) {
    threads.emplace_back([&, i] { servers[i] = std::make_unique<TestServer>(TIDESKV_SERVER_BIN); });
  }
  for (auto& t : threads) t.join();
  std::set<uint16_t> ports;
  for (auto& s : servers) {
    ports.insert(s->port());
    auto c = s->Connect();
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(
        c->Command({"INFO", "server"}).str.find("tcp_port:" + std::to_string(s->port())) != std::string::npos,
        true);
  }
  EXPECT_EQ(ports.size(), static_cast<size_t>(kServers));
}

// R17: a sanitizer report in a child server fails the test that owns the server, even when the child
// exits 0 (TSan's halt_on_error=0 keeps it running) or is killed with SIGKILL before it could exit 66.
TEST(TestServerIT, SanitizerReportInTheChildLogFailsTheTest) {
  TempDir dir("tideskv-fake-server");
  for (const char* marker : {"WARNING: ThreadSanitizer: data race (pid=1)",
                             "ERROR: AddressSanitizer: heap-use-after-free on address 0x1",
                             "ERROR: LeakSanitizer: detected memory leaks",
                             "SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior x.cc:1:1",
                             "ThreadSanitizer: CHECK failed: sanitizer_deadlock_detector.h:67",
                             // G6: TSan's own errors and fatal stops, and UBSan's per-site line
                             "ERROR: ThreadSanitizer: SEGV on unknown address 0x000000000000",
                             "FATAL: ThreadSanitizer: unexpected memory mapping 0x7f0000000000",
                             "src/x.cc:1:1: runtime error: signed integer overflow"}) {
    g_marker = marker;
    g_fake_server = WriteFakeServer(dir, "echo '" + g_marker + "' >&2");
    EXPECT_NONFATAL_FAILURE(
        {
          TestServer s(g_fake_server, {.wait_ready = false});
          ASSERT_TRUE(WaitForLog(s, g_marker));
        },
        "sanitizer report in tideskv-server");
  }
}

TEST(TestServerIT, KilledChildWithASanitizerReportStillFailsTheTest) {
  TempDir dir("tideskv-fake-server");
  g_marker = "WARNING: ThreadSanitizer: data race (pid=1)";
  g_fake_server = WriteFakeServer(dir, "echo '" + g_marker + "' >&2");
  EXPECT_NONFATAL_FAILURE(
      {
        TestServer s(g_fake_server, {.wait_ready = false});
        ASSERT_TRUE(WaitForLog(s, g_marker));
        s.Kill9();
      },
      "sanitizer report in tideskv-server");
}

// CF-M6b-11: TSan's deadlock detector aborts a thread holding more than 64 mutexes (an MSET of ~100 keys
// takes ~100 key stripes), so child servers run with detect_deadlocks=0; the inherited options are kept.
// The harness also passes the narrow suppressions file and clears DEBUGINFOD_URLS.
TEST(TestServerIT, ChildRunsWithTheHarnessSanitizerOptions) {
  TempDir dir("tideskv-fake-server");
  g_fake_server =
      WriteFakeServer(dir, "echo \"tsan=[$TSAN_OPTIONS] debuginfod=[${DEBUGINFOD_URLS-unset}]\" >&2");
  TestServer s(g_fake_server, {.wait_ready = false, .env = {{"TSAN_OPTIONS", "verbosity=0"}}});
  ASSERT_TRUE(WaitForLog(s, "tsan=["));
  const std::string log = s.Stderr();
  EXPECT_NE(log.find("tsan=[verbosity=0:"), std::string::npos) << log;
  EXPECT_NE(log.find("detect_deadlocks=0"), std::string::npos) << log;
  // The narrow suppressions for third-party code (tests/sanitizers/tsan.supp), as an existing file.
  const std::string supp = std::string("suppressions=") + TIDESKV_TSAN_SUPPRESSIONS;
  EXPECT_NE(log.find(supp), std::string::npos) << log;
  EXPECT_TRUE(std::filesystem::exists(TIDESKV_TSAN_SUPPRESSIONS));
  // A symbolizer must never wait on the network mid-report (with the gate held): no debuginfod.
  EXPECT_NE(log.find("debuginfod=[]"), std::string::npos) << log;
}

// The real server, started and stopped cleanly, leaves no sanitizer report (in the asan and tsan builds).
TEST(TestServerIT, CleanServerRunHasNoSanitizerReport) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"SET", "k", "v"}).str, "OK");
  EXPECT_EQ(s.Terminate(), 0);
  EXPECT_EQ(s.SanitizerReport(), "");
}

// CF-M6b-11: one command holding ~200 key stripes at once; without detect_deadlocks=0 a TSan child
// aborts here (sanitizer_deadlock_detector.h:67, more than 64 held locks).
TEST(TestServerIT, ManyKeyMsetRunsUnderEverySanitizer) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  std::vector<std::string> cmd = {"MSET"};
  for (int i = 0; i < 200; ++i) {
    cmd.push_back("k" + std::to_string(i));
    cmd.push_back("v");
  }
  EXPECT_EQ(c->Command(cmd).str, "OK");
  EXPECT_EQ(c->Command({"DBSIZE"}).integer, 200);
}

// The states of every thread of `pid` ('T' = stopped), from /proc/<pid>/task/<tid>/stat.
std::string ThreadStates(pid_t pid) {
  std::string states;
  for (const auto& task : std::filesystem::directory_iterator("/proc/" + std::to_string(pid) + "/task")) {
    std::ifstream in(task.path() / "stat");
    std::string stat((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t paren = stat.rfind(')');  // the comm field may hold spaces and parentheses
    if (paren != std::string::npos && paren + 2 < stat.size()) states += stat[paren + 2];
  }
  return states;
}

// SIGSTOP is asynchronous: a thread already running (a replication thread about to ACK) goes on until the
// stop reaches it. Pause must return only once every thread is stopped, or a test's "frozen" replica may
// still acknowledge the writes sent right after it (WaitIT under load).
TEST(TestServerIT, PauseReturnsOnceEveryThreadIsStopped) {
  TestServer s(TIDESKV_SERVER_BIN);
  for (int round = 0; round < 100; ++round) {
    s.Pause();
    const std::string states = ThreadStates(s.pid());
    ASSERT_EQ(states.find_first_not_of('T'), std::string::npos) << "round " << round << ": " << states;
    s.Resume();
  }
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"PING"}).str, "PONG");  // resumed
}

// A test whose TempDir dies before the server it backs (a test-local dir handed to a fixture-owned server)
// deletes the data dir of a running server: its next flush fails, and the background checkpoint then holds
// SIGTERM up to TidesDB's 60 s flush wait (ReplicationIT under load). The harness reports it.
TEST(TestServerIT, ADataDirRemovedWhileTheServerRunsFailsTheTest) {
  auto dir = std::make_unique<TempDir>("tideskv-it-data");
  g_data_dir = dir->str();
  EXPECT_NONFATAL_FAILURE(
      {
        TestServer s(TIDESKV_SERVER_BIN, {.dir = g_data_dir});
        std::filesystem::remove_all(g_data_dir);
      },
      "data dir was removed while the server ran");
}

}  // namespace
}  // namespace tideskv::test
