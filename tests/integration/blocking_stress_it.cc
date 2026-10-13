#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "support/it_helpers.h"
#include "support/test_server.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace tideskv::test {
namespace {

using Type = RespValue::Type;

// 16 waiters loop on BLPOP q with timeout 0 (a lost wakeup hangs instead of being masked by a re-arm)
// until they pop "STOP"; 8 pushers RPUSH 500 distinct values each. Every value must be popped exactly
// once, and a PING afterwards must answer PONG (a double reply would be read in its place).
TEST(BlockingStressIT, EveryPushedValueIsPoppedExactlyOnce) {
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--workers", "4"}});
  constexpr int kWaiters = 16, kPushers = 8, kPerPusher = 500;
  std::mutex mu;
  std::multiset<std::string> popped;
  std::atomic<int> errors{0};
  std::vector<std::thread> waiters;
  for (int w = 0; w < kWaiters; ++w) {
    waiters.emplace_back([&] {
      auto c = s.Connect();
      if (!c) return void(++errors);
      for (;;) {
        const RespValue r = c->Command({"BLPOP", "q", "0"});
        if (r.type != Type::kArray || r.elements.size() != 2) return void(++errors);
        if (r.elements[1].str == "STOP") break;
        std::lock_guard lock(mu);
        popped.insert(r.elements[1].str);
      }
      if (c->Command({"PING"}).str != "PONG") ++errors;
    });
  }
  std::vector<std::thread> pushers;
  for (int p = 0; p < kPushers; ++p) {
    pushers.emplace_back([&, p] {
      auto c = s.Connect();
      if (!c) return void(++errors);
      for (int i = 0; i < kPerPusher; ++i) {
        if (c->Command({"RPUSH", "q", std::to_string(p) + ":" + std::to_string(i)}).type != Type::kInteger)
          ++errors;
      }
    });
  }
  for (auto& t : pushers) t.join();
  {
    auto c = s.Connect();
    ASSERT_NE(c, nullptr);
    for (int w = 0; w < kWaiters; ++w) c->Command({"RPUSH", "q", "STOP"});
  }
  for (auto& t : waiters) t.join();
  EXPECT_EQ(errors.load(), 0);
  EXPECT_EQ(popped.size(), static_cast<size_t>(kPushers * kPerPusher));
  EXPECT_EQ(std::set<std::string>(popped.begin(), popped.end()).size(), popped.size());  // no duplicates
  auto c = s.Connect();
  EXPECT_EQ(c->Command({"LLEN", "q"}).integer, 0);
}

// Waiters with 10 ms timeouts race the pushers: a timed-out waiter must never consume a value.
TEST(BlockingStressIT, TimeoutsNeverLoseValues) {
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--workers", "4"}});
  std::atomic<bool> stop{false};
  std::atomic<int64_t> got{0};
  std::atomic<int> errors{0};
  std::vector<std::thread> waiters;
  for (int w = 0; w < 8; ++w) {
    waiters.emplace_back([&] {
      auto c = s.Connect();
      if (!c) return void(++errors);
      while (!stop) {
        const RespValue r = c->Command({"BLPOP", "q", "0.01"});
        if (r.type == Type::kArray) {
          ++got;
        } else if (r.type != Type::kNull) {
          ++errors;
        }
      }
    });
  }
  constexpr int kPushed = 4000;
  {
    auto c = s.Connect();
    ASSERT_NE(c, nullptr);
    for (int i = 0; i < kPushed; ++i) c->Command({"RPUSH", "q", std::to_string(i)});
  }
  stop = true;
  for (auto& t : waiters) t.join();
  auto c = s.Connect();
  const int64_t left = c->Command({"LLEN", "q"}).integer;
  EXPECT_EQ(errors.load(), 0);
  EXPECT_EQ(got.load() + left, kPushed);
}

TEST(BlockingStressIT, ZsetWaitersGetDistinctMembers) {
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--workers", "4"}});
  constexpr int kWaiters = 8;
  std::atomic<int> errors{0};
  std::mutex mu;
  std::set<std::string> got;
  std::vector<std::thread> waiters;
  for (int w = 0; w < kWaiters; ++w) {
    waiters.emplace_back([&] {
      auto c = s.Connect();
      if (!c) return void(++errors);
      const RespValue r = c->Command({"BZPOPMIN", "z", "0"});
      if (r.type != Type::kArray || r.elements.size() != 3) return void(++errors);
      if (c->Command({"PING"}).str != "PONG") ++errors;
      std::lock_guard lock(mu);
      got.insert(r.elements[1].str);
    });
  }
  auto p = s.Connect();
  // EXPECT, not ASSERT: push anyway, the waiters have no timeout and must be released.
  EXPECT_TRUE(WaitFor([&] { return InfoField(*p, "clients", "blocked_clients") == kWaiters; }));
  for (int i = 0; i < kWaiters; ++i) p->Command({"ZADD", "z", std::to_string(i), "m" + std::to_string(i)});
  for (auto& t : waiters) t.join();
  EXPECT_EQ(errors.load(), 0);
  EXPECT_EQ(got.size(), static_cast<size_t>(kWaiters));
}

}  // namespace
}  // namespace tideskv::test
