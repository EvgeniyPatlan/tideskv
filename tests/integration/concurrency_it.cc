#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <set>
#include <string>
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
  std::atomic<bool> reader_ready{false};
  std::atomic<int> torn{0};
  std::atomic<int> errors{0};
  std::atomic<int> reads{0};
  std::atomic<int> non_null_reads{0};
  std::set<std::string> distinct;  // touched only by the reader thread until it is joined
  std::thread reader([&] {
    auto c = s.Connect();
    if (!c) {
      ++errors;
      reader_ready = true;
      return;
    }
    reader_ready = true;
    while (!stop) {
      const RespValue r = c->Command({"MGET", "a", "b"});
      if (r.elements.size() != 2) {
        ++errors;
        continue;
      }
      ++reads;
      if (r.elements[0].str != r.elements[1].str) ++torn;
      if (r.elements[0].type != RespValue::Type::kNull) {
        ++non_null_reads;
        distinct.insert(r.elements[0].str);
      }
    }
  });
  std::thread writer([&] {
    auto c = s.Connect();
    while (!reader_ready) std::this_thread::yield();
    if (!c) {
      ++errors;
      stop = true;
      return;
    }
    // Keep writing until the reader has demonstrably observed values (bounded by a deadline).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (int i = 0; i < 2000 || (non_null_reads.load() < 20 && std::chrono::steady_clock::now() < deadline);
         ++i) {
      const std::string v = std::to_string(i);
      if (c->Command({"MSET", "a", v, "b", v}).str != "OK") ++errors;
    }
    stop = true;
  });
  writer.join();
  reader.join();
  EXPECT_EQ(errors.load(), 0);
  EXPECT_EQ(torn.load(), 0);
  EXPECT_GT(reads.load(), 0);
  EXPECT_GE(non_null_reads.load(), 20);
  EXPECT_GT(distinct.size(), 1u);
}

TEST(ConcurrencyIT, FlushAllUnderWriteLoadStaysConsistent) {
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--workers", "4"}});
  std::atomic<bool> stop{false};
  std::atomic<int> errors{0};
  std::vector<std::thread> writers;
  for (int t = 0; t < 4; ++t) {
    writers.emplace_back([&, t] {
      auto c = s.Connect();
      if (!c) {
        ++errors;
        return;
      }
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
