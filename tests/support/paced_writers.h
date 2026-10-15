#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

#include "common/status.h"

namespace tideskv::test {

// Concurrent committers that the test paces: together they make at most Grant()'s total of commit calls,
// so a test can take a cut while commits are still in flight without the store growing unboundedly.
// commit(writer, i) makes writer's i-th commit; failures() counts the calls that did not return Ok.
class PacedWriters {
 public:
  using CommitFn = std::function<Status(int writer, uint64_t i)>;
  PacedWriters(int writers, CommitFn commit) : commit_(std::move(commit)) {
    for (int w = 0; w < writers; ++w) threads_.emplace_back([this, w] { Run(w); });
  }
  ~PacedWriters() {
    stop_.store(true);
    for (std::thread& t : threads_) t.join();
  }
  PacedWriters(const PacedWriters&) = delete;
  PacedWriters& operator=(const PacedWriters&) = delete;

  void Grant(uint64_t total) { budget_.store(total); }
  int failures() const { return failures_.load(); }

 private:
  void Run(int w) {
    for (uint64_t i = 0; !stop_.load();) {
      uint64_t n = issued_.load();
      if (n >= budget_.load()) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        continue;
      }
      if (!issued_.compare_exchange_weak(n, n + 1)) continue;
      if (!commit_(w, i++).ok()) failures_.fetch_add(1);
    }
  }

  CommitFn commit_;
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> budget_{0};
  std::atomic<uint64_t> issued_{0};
  std::atomic<int> failures_{0};
  std::vector<std::thread> threads_;
};

}  // namespace tideskv::test
