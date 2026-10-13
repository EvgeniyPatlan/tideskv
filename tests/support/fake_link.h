#pragma once
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "server/session.h"

namespace tideskv::test {

// Records the replies delivered to a blocked client (thread-safe).
class FakeLink final : public ClientLink {
 public:
  void Unblock(std::string reply) override {
    std::lock_guard lock(mu_);
    replies_.push_back(std::move(reply));
  }
  std::vector<std::string> Take() {
    std::lock_guard lock(mu_);
    return std::exchange(replies_, {});
  }

 private:
  std::mutex mu_;
  std::vector<std::string> replies_;
};

}  // namespace tideskv::test
