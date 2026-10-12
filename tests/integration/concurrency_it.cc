#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "support/test_server.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace tideskv::test {
namespace {

TEST(ConcurrencyIT, ParallelIncrIsExact) {  // Review Focus #2
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--workers", "4"}});
  std::vector<std::thread> threads;
  std::atomic<int> errors{0};
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      auto c = s.Connect();
      if (!c) return void(++errors);
      for (int i = 0; i < 1000; ++i) {
        if (c->Command({"INCR", "ctr"}).type != RespValue::Type::kInteger) ++errors;
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(errors.load(), 0);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"GET", "ctr"}).str, "8000");
}

TEST(ConcurrencyIT, MsetIsNeverSeenHalfApplied) {  // Review Focus #2
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--workers", "4"}});
  std::atomic<bool> stop{false};
  std::atomic<int> torn{0};
  std::thread writer([&] {
    auto c = s.Connect();
    for (int i = 0; i < 2000; ++i) {
      const std::string v = std::to_string(i);
      c->Command({"MSET", "a", v, "b", v});
    }
    stop = true;
  });
  std::thread reader([&] {
    auto c = s.Connect();
    while (!stop) {
      const RespValue r = c->Command({"MGET", "a", "b"});
      if (r.elements.size() == 2 && r.elements[0].str != r.elements[1].str) ++torn;
    }
  });
  writer.join();
  reader.join();
  EXPECT_EQ(torn.load(), 0);
}

TEST(ConcurrencyIT, FlushAllUnderWriteLoadStaysConsistent) {
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--workers", "4"}});
  std::atomic<bool> stop{false};
  std::atomic<int> errors{0};
  std::vector<std::thread> writers;
  for (int t = 0; t < 4; ++t) {
    writers.emplace_back([&, t] {
      auto c = s.Connect();
      while (!stop) {
        if (c->Command({"INCR", "w" + std::to_string(t)}).type != RespValue::Type::kInteger) ++errors;
      }
    });
  }
  auto admin = s.Connect();
  ASSERT_NE(admin, nullptr);
  for (int i = 0; i < 20; ++i) EXPECT_EQ(admin->Command({"FLUSHALL"}).str, "OK");
  stop = true;
  for (auto& w : writers) w.join();
  EXPECT_EQ(errors.load(), 0);
  EXPECT_EQ(admin->Command({"PING"}).str, "PONG");
}

}  // namespace
}  // namespace tideskv::test
