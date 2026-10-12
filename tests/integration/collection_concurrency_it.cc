#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "support/test_server.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace tideskv::test {
namespace {

using Type = RespValue::Type;

TEST(CollectionConcurrencyIT, ParallelHincrbyIsExact) {
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--workers", "4"}});
  std::atomic<int> errors{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      auto c = s.Connect();
      if (!c) return void(++errors);
      for (int i = 0; i < 1000; ++i) {
        if (c->Command({"HINCRBY", "h", "f", "1"}).type != Type::kInteger) ++errors;
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(errors.load(), 0);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"HGET", "h", "f"}).str, "8000");
}

TEST(CollectionConcurrencyIT, ParallelSaddCountsEveryMember) {
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--workers", "4"}});
  std::atomic<int> errors{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      auto c = s.Connect();
      if (!c) return void(++errors);
      for (int i = 0; i < 500; ++i) {
        if (c->Command({"SADD", "s", std::to_string(t) + ":" + std::to_string(i)}).integer != 1) ++errors;
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(errors.load(), 0);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"SCARD", "s"}).integer, 4000);
}

TEST(CollectionConcurrencyIT, SmovePingPongIsNeverSeenHalfDone) {
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--workers", "4"}});
  {
    auto c = s.Connect();
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->Command({"SADD", "s1", "m", "anchor1"}).integer, 2);
    ASSERT_EQ(c->Command({"SADD", "s2", "anchor2"}).integer, 1);
  }
  std::atomic<bool> stop{false};
  std::atomic<int> errors{0}, both{0}, neither{0}, checks{0};
  std::thread mover([&] {
    auto c = s.Connect();
    if (!c) return void(++errors);
    for (int i = 0; i < 2000; ++i) {
      if (c->Command({"SMOVE", "s1", "s2", "m"}).type != Type::kInteger) ++errors;
      if (c->Command({"SMOVE", "s2", "s1", "m"}).type != Type::kInteger) ++errors;
    }
    stop = true;
  });
  std::thread checker([&] {
    auto c = s.Connect();
    if (!c) return void(++errors);
    while (!stop) {
      // SINTER and SUNION each read one snapshot of both sets.
      const RespValue inter = c->Command({"SINTER", "s1", "s2"});
      const RespValue uni = c->Command({"SUNION", "s1", "s2"});
      int seen = 0;
      for (const RespValue& e : uni.elements) seen += e.str == "m" ? 1 : 0;
      if (!inter.elements.empty()) ++both;
      if (seen == 0) ++neither;
      ++checks;
    }
  });
  mover.join();
  checker.join();
  EXPECT_EQ(errors.load(), 0);
  EXPECT_EQ(both.load(), 0);
  EXPECT_EQ(neither.load(), 0);
  EXPECT_GT(checks.load(), 0);
}

TEST(CollectionConcurrencyIT, ConcurrentCreatesCrossALeaseBoundary) {
  constexpr int kClients = 8;
  constexpr int kPerClient = 9000;  // 72000 creations > OidAllocator::kLeaseSize (65536)
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--workers", "4"}});
  std::atomic<int> errors{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kClients; ++t) {
    threads.emplace_back([&, t] {
      auto c = s.Connect();
      if (!c) return void(++errors);
      for (int i = 0; i < kPerClient; ++i) {
        const RespValue r = c->Command({"HSET", "c" + std::to_string(t) + ":" + std::to_string(i), "f", "v"});
        if (r.type != Type::kInteger || r.integer != 1) ++errors;
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(errors.load(), 0);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"DBSIZE"}).integer, kClients * kPerClient);
  for (int n = 0; n < kClients * kPerClient; n += 997) {
    const std::string key = "c" + std::to_string(n / kPerClient) + ":" + std::to_string(n % kPerClient);
    EXPECT_EQ(c->Command({"HGETALL", key}).elements.size(), 2u) << key;
  }
}

}  // namespace
}  // namespace tideskv::test
