#include "exec/gate.h"

namespace tideskv {

void Gate::lock_shared() {
  std::unique_lock l(mu_);
  cv_.wait(l, [this] { return !writer_ && writers_waiting_ == 0; });
  ++readers_;
}

void Gate::unlock_shared() {
  std::lock_guard l(mu_);
  if (--readers_ == 0) cv_.notify_all();
}

void Gate::lock() {
  std::unique_lock l(mu_);
  ++writers_waiting_;
  cv_.wait(l, [this] { return !writer_ && readers_ == 0; });
  --writers_waiting_;
  writer_ = true;
}

bool Gate::try_lock() {
  std::lock_guard l(mu_);
  if (writer_ || readers_ != 0) return false;
  writer_ = true;
  return true;
}

bool Gate::try_lock_shared() {
  std::lock_guard l(mu_);
  if (writer_ || writers_waiting_ != 0) return false;
  ++readers_;
  return true;
}

bool Gate::lock_shared_unless(const std::function<bool()>& give_up) {
  std::unique_lock l(mu_);
  while (writer_ || writers_waiting_ != 0) {
    if (give_up()) return false;
    cv_.wait(l);
  }
  ++readers_;
  return true;
}

bool Gate::lock_unless(const std::function<bool()>& give_up) {
  std::unique_lock l(mu_);
  ++writers_waiting_;
  while (writer_ || readers_ != 0) {
    if (give_up()) {
      if (--writers_waiting_ == 0) cv_.notify_all();  // readers queued behind us may proceed
      return false;
    }
    cv_.wait(l);
  }
  --writers_waiting_;
  writer_ = true;
  return true;
}

void Gate::Wake() {
  std::lock_guard l(mu_);
  cv_.notify_all();
}

void Gate::unlock() {
  std::lock_guard l(mu_);
  writer_ = false;
  cv_.notify_all();
}

}  // namespace tideskv
