#pragma once
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "server/session.h"

namespace tideskv::test {

// Records the replies delivered to a blocked client, and how many closed the connection (thread-safe).
class FakeLink final : public ClientLink {
 public:
  void Unblock(std::string reply) override {
    std::lock_guard lock(mu_);
    replies_.push_back(std::move(reply));
  }
  void UnblockAndClose(std::string reply) override {
    std::lock_guard lock(mu_);
    replies_.push_back(std::move(reply));
    ++closes_;
  }
  int closes() {
    std::lock_guard lock(mu_);
    return closes_;
  }
  std::vector<std::string> Take() {
    std::lock_guard lock(mu_);
    return std::exchange(replies_, {});
  }

 private:
  std::mutex mu_;
  std::vector<std::string> replies_;
  int closes_ = 0;  // replies delivered with UnblockAndClose
};

}  // namespace tideskv::test
