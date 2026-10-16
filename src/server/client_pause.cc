#include "server/client_pause.h"

#include <algorithm>
#include <utility>

namespace tideskv {

ClientPause::ClientPause() : timer_([this] { TimerLoop(); }) {}

ClientPause::~ClientPause() { Shutdown(); }

int64_t ClientPause::ToNs(Clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

// mode_ is stored before end_ns_ and read after it, so a reader that sees an end time sees its mode.
ClientPause::Mode ClientPause::mode() const {
  const int64_t end = end_ns_.load(std::memory_order_acquire);
  if (end == 0 || ToNs(Clock::now()) >= end) return Mode::kOff;
  return static_cast<Mode>(mode_.load(std::memory_order_acquire));
}

void ClientPause::Pause(Clock::time_point end, Mode mode) {
  std::vector<std::weak_ptr<ClientLink>> stale;  // parked under a pause that ended before the timer ran
  {
    std::lock_guard lock(mu_);
    const int64_t ns = ToNs(end);
    const auto m = static_cast<uint8_t>(mode);
    if (this->mode() == Mode::kOff) {
      stale.swap(parked_);
      mode_.store(m, std::memory_order_release);
      end_ns_.store(ns, std::memory_order_release);
    } else {
      mode_.store(std::max(mode_.load(std::memory_order_relaxed), m), std::memory_order_release);
      end_ns_.store(std::max(end_ns_.load(std::memory_order_relaxed), ns), std::memory_order_release);
    }
  }
  cv_.notify_all();
  ResumeAll(stale);  // each re-dispatches its command, which the new pause may park again
}

void ClientPause::Unpause() {
  std::vector<std::weak_ptr<ClientLink>> links;
  {
    std::lock_guard lock(mu_);
    end_ns_.store(0, std::memory_order_release);
    mode_.store(0, std::memory_order_release);
    links.swap(parked_);
  }
  cv_.notify_all();
  ResumeAll(links);
}

bool ClientPause::Park(std::weak_ptr<ClientLink> link) {
  std::lock_guard lock(mu_);
  if (stop_ || mode() == Mode::kOff) return false;
  parked_.push_back(std::move(link));
  return true;
}

size_t ClientPause::parked() const {
  std::lock_guard lock(mu_);
  return static_cast<size_t>(
      std::count_if(parked_.begin(), parked_.end(), [](const auto& weak) { return !weak.expired(); }));
}

void ClientPause::Shutdown() {
  {
    std::lock_guard lock(mu_);
    stop_ = true;
    parked_.clear();
  }
  cv_.notify_all();
  if (timer_.joinable() && timer_.get_id() != std::this_thread::get_id()) timer_.join();
}

// Called without mu_, which stays a leaf; Resume only posts to the connection's executor.
void ClientPause::ResumeAll(const std::vector<std::weak_ptr<ClientLink>>& links) {
  for (const auto& weak : links) {
    if (auto link = weak.lock()) link->Resume();
  }
}

// Ends a pause at its end time: resumes the parked clients exactly as Unpause does.
void ClientPause::TimerLoop() {
  std::unique_lock lock(mu_);
  while (!stop_) {
    const int64_t end = end_ns_.load(std::memory_order_acquire);
    if (end == 0) {
      cv_.wait(lock);
      continue;
    }
    const Clock::time_point deadline{std::chrono::nanoseconds(end)};
    if (Clock::now() < deadline) {
      cv_.wait_until(lock, deadline);
      continue;
    }
    end_ns_.store(0, std::memory_order_release);
    mode_.store(0, std::memory_order_release);
    auto links = std::exchange(parked_, {});
    lock.unlock();
    ResumeAll(links);
    lock.lock();
  }
}

}  // namespace tideskv
