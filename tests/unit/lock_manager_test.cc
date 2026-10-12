#include "exec/lock_manager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace tideskv {
namespace {

constexpr auto kTimeout = std::chrono::seconds(2);

TEST(LockManager, StripeIsDeterministicAndInRange) {
  for (int i = 0; i < 1000; ++i) {
    const std::string key = "key:" + std::to_string(i);
    EXPECT_EQ(LockManager::StripeOf(0, key), LockManager::StripeOf(0, key));
    EXPECT_LT(LockManager::StripeOf(static_cast<uint8_t>(i % 16), key), LockManager::kStripes);
  }
}

TEST(LockManager, DuplicateKeysDoNotSelfDeadlock) {
  LockManager lm;
  auto fut = std::async(std::launch::async, [&] { auto guard = lm.Lock(0, {"a", "a", "b", "a"}); });
  ASSERT_EQ(fut.wait_for(kTimeout), std::future_status::ready);
}

TEST(LockManager, EmptyKeyListIsAllowed) {
  LockManager lm;
  auto fut = std::async(std::launch::async, [&] { auto guard = lm.Lock(0, {}); });
  ASSERT_EQ(fut.wait_for(kTimeout), std::future_status::ready);
}

TEST(LockManager, SameKeyIsMutuallyExclusive) {
  LockManager lm;
  int counter = 0;  // deliberately not atomic: TSan proves the lock protects it
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 10000; ++i) {
        auto guard = lm.Lock(3, {"hot"});
        ++counter;
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(counter, 80000);
}

TEST(LockManager, OppositeKeyOrderCannotDeadlock) {
  LockManager lm;
  auto run = [&](std::vector<std::string_view> keys) {
    for (int i = 0; i < 20000; ++i) auto guard = lm.Lock(0, keys);
  };
  auto a = std::async(std::launch::async, run, std::vector<std::string_view>{"x", "y", "z"});
  auto b = std::async(std::launch::async, run, std::vector<std::string_view>{"z", "y", "x"});
  ASSERT_EQ(a.wait_for(std::chrono::seconds(20)), std::future_status::ready);
  ASSERT_EQ(b.wait_for(std::chrono::seconds(20)), std::future_status::ready);
}

TEST(LockManager, DifferentStripesDoNotBlockEachOther) {
  LockManager lm;
  std::string other = "b";
  while (LockManager::StripeOf(0, other) == LockManager::StripeOf(0, "a")) other += "b";
  auto held = lm.Lock(0, {"a"});
  auto fut = std::async(std::launch::async, [&] { auto g = lm.Lock(0, {other}); });
  EXPECT_EQ(fut.wait_for(kTimeout), std::future_status::ready);
}

TEST(LockManager, MovedGuardUnlocksExactlyOnce) {
  LockManager lm;
  {
    auto g1 = lm.Lock(0, {"k"});
    LockManager::Guard g2(std::move(g1));
  }  // g2 unlocks; g1 must not unlock again (that would be UB on std::mutex)
  auto fut = std::async(std::launch::async, [&] { auto g = lm.Lock(0, {"k"}); });
  ASSERT_EQ(fut.wait_for(kTimeout), std::future_status::ready);
}

}  // namespace
}  // namespace tideskv
