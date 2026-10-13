#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

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
    EXPECT_EQ(c->Command({"INFO", "server"}).str.find("tcp_port:" + std::to_string(s->port())) != std::string::npos,
              true);
  }
  EXPECT_EQ(ports.size(), static_cast<size_t>(kServers));
}

}  // namespace
}  // namespace tideskv::test
