#include "exec/gate.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

namespace tideskv {
namespace {

TEST(Gate, SharedHoldersOverlap) {
  Gate gate;
  std::shared_lock a(gate);
  auto fut = std::async(std::launch::async, [&] { std::shared_lock b(gate); });
  EXPECT_EQ(fut.wait_for(std::chrono::seconds(2)), std::future_status::ready);
}

TEST(Gate, ExclusiveWaitsForReaders) {
  Gate gate;
  std::atomic<bool> writer_in{false};
  std::unique_ptr<std::shared_lock<Gate>> reader = std::make_unique<std::shared_lock<Gate>>(gate);
  std::thread writer([&] {
    std::unique_lock w(gate);
    writer_in = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(writer_in.load());
  reader.reset();
  writer.join();
  EXPECT_TRUE(writer_in.load());
}

TEST(Gate, WaitingWriterIsNotStarvedByReaders) {
  Gate gate;
  std::atomic<bool> stop{false};
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back([&] {
      while (!stop) {
        std::shared_lock r(gate);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
    });
  }
  auto fut = std::async(std::launch::async, [&] { std::unique_lock w(gate); });
  EXPECT_EQ(fut.wait_for(std::chrono::seconds(3)), std::future_status::ready);
  stop = true;
  for (auto& t : readers) t.join();
}

TEST(Gate, WriterExcludesWriter) {
  Gate gate;
  int counter = 0;
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 5000; ++i) {
        std::unique_lock w(gate);
        ++counter;
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(counter, 20000);
}

TEST(Gate, TryLockFailsWhileAnyHolderExists) {
  Gate gate;
  {
    std::shared_lock r(gate);
    EXPECT_FALSE(gate.try_lock());
  }
  ASSERT_TRUE(gate.try_lock());
  EXPECT_FALSE(gate.try_lock());
  gate.unlock();
  EXPECT_TRUE(gate.try_lock());
  gate.unlock();
}

TEST(Gate, TryLockSharedFailsWhileAWriterHoldsOrWaits) {
  Gate gate;
  ASSERT_TRUE(gate.try_lock_shared());
  EXPECT_TRUE(gate.try_lock_shared());  // readers overlap
  std::atomic<bool> writer_in{false};
  std::thread writer([&] {
    std::unique_lock w(gate);  // queues behind the two readers
    writer_in = true;
  });
  // Once the writer is waiting, new readers are refused (writer priority).
  while (gate.try_lock_shared()) {
    gate.unlock_shared();
    std::this_thread::yield();
  }
  EXPECT_FALSE(writer_in.load());
  gate.unlock_shared();
  gate.unlock_shared();
  writer.join();
  EXPECT_TRUE(writer_in.load());
  {
    std::unique_lock w(gate);
    EXPECT_FALSE(gate.try_lock_shared());
  }
  EXPECT_TRUE(gate.try_lock_shared());
  gate.unlock_shared();
}

}  // namespace
}  // namespace tideskv
