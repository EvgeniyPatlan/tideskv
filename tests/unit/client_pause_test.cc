#include "server/client_pause.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <utility>

#include "support/fake_link.h"

namespace tideskv {
namespace {

using namespace std::chrono_literals;
using Mode = ClientPause::Mode;
using Admission = ClientPause::Admission;

bool WaitFor(const std::function<bool()>& cond, std::chrono::milliseconds timeout = 2s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (cond()) return true;
    std::this_thread::sleep_for(5ms);
  }
  return cond();
}

TEST(ClientPause, WriteModeBlocksOnlyMayReplicateCommandsUntilTheEnd) {
  ClientPause p;
  EXPECT_FALSE(p.Blocks(true));
  p.Pause(ClientPause::Clock::now() + 200ms, Mode::kWrite);
  EXPECT_EQ(p.mode(), Mode::kWrite);
  EXPECT_TRUE(p.Blocks(true));
  EXPECT_FALSE(p.Blocks(false));
  EXPECT_TRUE(WaitFor([&] { return p.mode() == Mode::kOff; }));
  EXPECT_FALSE(p.Blocks(true));
}

TEST(ClientPause, ANewPauseKeepsTheLaterEndAndTheStricterMode) {  // pauseClientsByClient
  ClientPause p;
  p.Pause(ClientPause::Clock::now() + 10s, Mode::kAll);
  p.Pause(ClientPause::Clock::now() + 20ms, Mode::kWrite);
  std::this_thread::sleep_for(60ms);  // past the second end: the first one must still hold
  EXPECT_EQ(p.mode(), Mode::kAll);
  EXPECT_TRUE(p.Blocks(false));
  p.Unpause();
  EXPECT_EQ(p.mode(), Mode::kOff);
}

TEST(ClientPause, UnpauseResumesEveryParkedLinkOnce) {
  ClientPause p;
  auto a = std::make_shared<test::FakeLink>();
  auto b = std::make_shared<test::FakeLink>();
  p.Pause(ClientPause::Clock::now() + 10s, Mode::kWrite);
  ASSERT_EQ(p.Admit(true, a), Admission::kParked);
  ASSERT_EQ(p.Admit(true, b), Admission::kParked);
  EXPECT_EQ(p.parked(), 2u);
  p.Unpause();
  EXPECT_EQ(a->resumes(), 1);
  EXPECT_EQ(b->resumes(), 1);
  EXPECT_EQ(p.parked(), 0u);
  EXPECT_EQ(p.Admit(true, a), Admission::kRunCounted);  // no pause: the caller runs the command now
  p.EndWrite();
}

TEST(ClientPause, TheEndTimeResumesParkedLinks) {
  ClientPause p;
  auto a = std::make_shared<test::FakeLink>();
  p.Pause(ClientPause::Clock::now() + 100ms, Mode::kAll);
  ASSERT_EQ(p.Admit(true, a), Admission::kParked);
  EXPECT_TRUE(WaitFor([&] { return a->resumes() == 1; }));
  EXPECT_EQ(p.parked(), 0u);
}

TEST(ClientPause, AGoneLinkIsSkippedAndShutdownResumesNobody) {
  ClientPause p;
  auto a = std::make_shared<test::FakeLink>();
  p.Pause(ClientPause::Clock::now() + 10s, Mode::kAll);
  ASSERT_EQ(p.Admit(true, std::make_shared<test::FakeLink>()),
            Admission::kParked);  // destroyed at once: its weak_ptr expires
  ASSERT_EQ(p.Admit(true, a), Admission::kParked);
  EXPECT_EQ(p.parked(), 1u);  // a closed client is not counted in INFO blocked_clients
  p.Shutdown();
  EXPECT_EQ(a->resumes(), 0);
}

TEST(ClientPause, UnpauseSkipsAGoneLinkAndResumesTheOthers) {
  ClientPause p;
  auto a = std::make_shared<test::FakeLink>();
  p.Pause(ClientPause::Clock::now() + 10s, Mode::kWrite);
  ASSERT_EQ(p.Admit(true, std::make_shared<test::FakeLink>()),
            Admission::kParked);  // expired before the resume
  ASSERT_EQ(p.Admit(true, a), Admission::kParked);
  p.Unpause();
  EXPECT_EQ(a->resumes(), 1);
  EXPECT_EQ(p.parked(), 0u);
}

TEST(ClientPause, AReadIsNeverParkedUnderAWritePause) {  // one decision under mu_ (review fix 1, #2)
  ClientPause p;
  auto a = std::make_shared<test::FakeLink>();
  p.Pause(ClientPause::Clock::now() + 10s, Mode::kWrite);
  EXPECT_EQ(p.Admit(false, a), Admission::kRun);
  EXPECT_EQ(p.parked(), 0u);
  p.Pause(ClientPause::Clock::now() + 10s, Mode::kAll);
  EXPECT_EQ(p.Admit(false, a), Admission::kParked);
}

// Runs fn on a thread; done() turns true once it returned.
class Background {
 public:
  explicit Background(std::function<void()> fn)
      : thread_([this, fn = std::move(fn)] {
          fn();
          done_ = true;
        }) {}
  ~Background() { thread_.join(); }
  Background(const Background&) = delete;
  Background& operator=(const Background&) = delete;
  bool done() const { return done_; }

 private:
  std::atomic<bool> done_{false};
  std::thread thread_;
};

// D21 barrier: nothing admitted before the pause may commit after CLIENT PAUSE replies.
TEST(ClientPause, WaitForWritesWaitsForAWriteAdmittedBeforeThePause) {
  ClientPause p;
  auto a = std::make_shared<test::FakeLink>();
  ASSERT_EQ(p.Admit(true, a), Admission::kRunCounted);
  p.Pause(ClientPause::Clock::now() + 10s, Mode::kWrite);
  Background waiter([&] { p.WaitForWrites(0); });
  std::this_thread::sleep_for(100ms);
  EXPECT_FALSE(waiter.done());
  p.EndWrite();
  EXPECT_TRUE(WaitFor([&] { return waiter.done(); }));
}

TEST(ClientPause, WaitForWritesLeavesOutTheCallersOwnCommand) {  // CLIENT PAUSE inside an EXEC with writes
  ClientPause p;
  auto a = std::make_shared<test::FakeLink>();
  ASSERT_EQ(p.Admit(true, a), Admission::kRunCounted);
  p.Pause(ClientPause::Clock::now() + 10s, Mode::kWrite);
  p.WaitForWrites(1);  // returns at once
  p.EndWrite();
  p.WaitForWrites(0);
}

TEST(ClientPause, ABackgroundWriteIsRefusedWhilePausedAndWaitedFor) {  // the expire reaper
  ClientPause p;
  ASSERT_TRUE(p.BeginBackgroundWrite());
  p.Pause(ClientPause::Clock::now() + 10s, Mode::kWrite);
  EXPECT_FALSE(p.BeginBackgroundWrite());
  Background waiter([&] { p.WaitForWrites(0); });
  std::this_thread::sleep_for(100ms);
  EXPECT_FALSE(waiter.done());
  p.EndWrite();
  EXPECT_TRUE(WaitFor([&] { return waiter.done(); }));
  p.Unpause();
  EXPECT_TRUE(p.BeginBackgroundWrite());
  p.EndWrite();
}

// The guarantee is moot once the pause is gone: a waiter stuck behind a held write returns on UNPAUSE.
TEST(ClientPause, UnpauseReleasesAWaiterBehindAHeldWrite) {
  ClientPause p;
  auto a = std::make_shared<test::FakeLink>();
  ASSERT_EQ(p.Admit(true, a), Admission::kRunCounted);
  p.Pause(ClientPause::Clock::now() + 10s, Mode::kWrite);
  Background waiter([&] { p.WaitForWrites(0); });
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(waiter.done());
  p.Unpause();
  EXPECT_TRUE(WaitFor([&] { return waiter.done(); }));
  p.EndWrite();  // before ~Background: releases a waiter the Unpause did not
}

// ... and once the pause expires.
TEST(ClientPause, TheEndTimeReleasesAWaiterBehindAHeldWrite) {
  ClientPause p;
  auto a = std::make_shared<test::FakeLink>();
  ASSERT_EQ(p.Admit(true, a), Admission::kRunCounted);
  p.Pause(ClientPause::Clock::now() + 200ms, Mode::kWrite);
  Background waiter([&] { p.WaitForWrites(0); });
  EXPECT_TRUE(WaitFor([&] { return waiter.done(); }));
  EXPECT_EQ(p.mode(), Mode::kOff);
  p.EndWrite();  // before ~Background: releases a waiter the end time did not
}

TEST(ClientPause, ShutdownReleasesAWaiter) {
  ClientPause p;
  auto a = std::make_shared<test::FakeLink>();
  ASSERT_EQ(p.Admit(true, a), Admission::kRunCounted);
  p.Pause(ClientPause::Clock::now() + 10s, Mode::kAll);
  Background waiter([&] { p.WaitForWrites(0); });
  std::this_thread::sleep_for(50ms);
  p.Shutdown();
  EXPECT_TRUE(WaitFor([&] { return waiter.done(); }));
  p.EndWrite();
}

}  // namespace
}  // namespace tideskv
