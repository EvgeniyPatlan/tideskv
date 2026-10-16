#include "server/client_pause.h"

#include <algorithm>
#include <utility>

namespace tideskv {

ClientPause::ClientPause() : timer_([this] { TimerLoop(); }) {}

ClientPause::~ClientPause() { Shutdown(); }

int64_t ClientPause::ToNs(Clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

// mode_ is stored before end_ns_ and read after it, so a reader that sees an end time sees its mode. All
// accesses are seq_cst: Admit and WaitForWrites rely on one total order with in_flight_.
ClientPause::Mode ClientPause::mode() const {
  const int64_t end = end_ns_.load();
  if (end == 0 || ToNs(Clock::now()) >= end) return Mode::kOff;
  return static_cast<Mode>(mode_.load());
}

void ClientPause::Pause(Clock::time_point end, Mode mode) {
  std::vector<std::weak_ptr<ClientLink>> stale;  // parked under a pause that ended before the timer ran
  {
    std::lock_guard lock(mu_);
    const int64_t ns = ToNs(end);
    const auto m = static_cast<uint8_t>(mode);
    if (this->mode() == Mode::kOff) {
      stale.swap(parked_);
      mode_.store(m);
      end_ns_.store(ns);
    } else {
      mode_.store(std::max(mode_.load(), m));
      end_ns_.store(std::max(end_ns_.load(), ns));
    }
  }
  cv_.notify_all();
  ResumeAll(stale);  // each re-dispatches its command, which the new pause may park again
}

void ClientPause::Unpause() {
  std::vector<std::weak_ptr<ClientLink>> links;
  {
    std::lock_guard lock(mu_);
    end_ns_.store(0);
    mode_.store(0);
    links.swap(parked_);
  }
  cv_.notify_all();
  ResumeAll(links);
}

ClientPause::Admission ClientPause::Admit(bool may_replicate, std::weak_ptr<ClientLink> link) {
  if (!may_replicate) {
    if (mode() != Mode::kAll) return Admission::kRun;  // lock-free: reads never wait for a WRITE pause
    return ParkOrCount(false, std::move(link));
  }
  in_flight_.fetch_add(1);  // before the mode is read: a Pause() that this misses waits for it
  if (!Blocks(true)) return Admission::kRunCounted;
  EndWrite();
  return ParkOrCount(true, std::move(link));
}

// Pause() and Unpause() change the mode under mu_, so the decision taken here holds.
ClientPause::Admission ClientPause::ParkOrCount(bool may_replicate, std::weak_ptr<ClientLink> link) {
  std::lock_guard lock(mu_);
  if (!stop_ && Blocks(may_replicate)) {
    parked_.push_back(std::move(link));
    return Admission::kParked;
  }
  if (!may_replicate) return Admission::kRun;
  in_flight_.fetch_add(1);
  return Admission::kRunCounted;
}

bool ClientPause::BeginBackgroundWrite() {
  in_flight_.fetch_add(1);
  if (mode() == Mode::kOff) return true;
  EndWrite();
  return false;
}

void ClientPause::EndWrite() {
  in_flight_.fetch_sub(1);
  if (waiters_.load() > 0) {  // read after the decrement: a waiter that registers later sees it
    std::lock_guard lock(mu_);
    drained_.notify_all();
  }
}

// The caller's own writes are taken out while it waits, so two callers (two EXECs pausing at once) never
// wait for each other.
void ClientPause::WaitForWrites(int own) {
  std::unique_lock lock(mu_);
  waiters_.fetch_add(1);
  in_flight_.fetch_sub(own);
  drained_.wait(lock, [this] { return stop_ || in_flight_.load() <= 0; });
  in_flight_.fetch_add(own);
  waiters_.fetch_sub(1);
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
  drained_.notify_all();
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
    const int64_t end = end_ns_.load();
    if (end == 0) {
      cv_.wait(lock);
      continue;
    }
    const Clock::time_point deadline{std::chrono::nanoseconds(end)};
    if (Clock::now() < deadline) {
      cv_.wait_until(lock, deadline);
      continue;
    }
    end_ns_.store(0);
    mode_.store(0);
    auto links = std::exchange(parked_, {});
    lock.unlock();
    ResumeAll(links);
    lock.lock();
  }
}

}  // namespace tideskv
