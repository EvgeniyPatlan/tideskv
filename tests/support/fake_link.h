#pragma once
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "server/session.h"

namespace tideskv::test {

// Records the replies delivered to a blocked client, how many closed the connection and how many resumed a
// postponed command (thread-safe).
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
  void Resume() override {
    std::lock_guard lock(mu_);
    ++resumes_;
  }
  int resumes() {
    std::lock_guard lock(mu_);
    return resumes_;
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
  int closes_ = 0;   // replies delivered with UnblockAndClose
  int resumes_ = 0;  // Resume calls (CLIENT PAUSE ended)
};

}  // namespace tideskv::test
