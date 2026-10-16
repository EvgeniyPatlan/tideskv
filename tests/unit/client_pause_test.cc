#include "server/client_pause.h"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <thread>

#include "support/fake_link.h"

namespace tideskv {
namespace {

using namespace std::chrono_literals;
using Mode = ClientPause::Mode;

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
  ASSERT_TRUE(p.Park(a));
  ASSERT_TRUE(p.Park(b));
  EXPECT_EQ(p.parked(), 2u);
  p.Unpause();
  EXPECT_EQ(a->resumes(), 1);
  EXPECT_EQ(b->resumes(), 1);
  EXPECT_EQ(p.parked(), 0u);
  EXPECT_FALSE(p.Park(a));  // no pause: the caller runs the command now
}

TEST(ClientPause, TheEndTimeResumesParkedLinks) {
  ClientPause p;
  auto a = std::make_shared<test::FakeLink>();
  p.Pause(ClientPause::Clock::now() + 100ms, Mode::kAll);
  ASSERT_TRUE(p.Park(a));
  EXPECT_TRUE(WaitFor([&] { return a->resumes() == 1; }));
  EXPECT_EQ(p.parked(), 0u);
}

TEST(ClientPause, AGoneLinkIsSkippedAndShutdownResumesNobody) {
  ClientPause p;
  auto a = std::make_shared<test::FakeLink>();
  p.Pause(ClientPause::Clock::now() + 10s, Mode::kAll);
  ASSERT_TRUE(p.Park(std::make_shared<test::FakeLink>()));  // destroyed at once: its weak_ptr expires
  ASSERT_TRUE(p.Park(a));
  EXPECT_EQ(p.parked(), 1u);  // a closed client is not counted in INFO blocked_clients
  p.Shutdown();
  EXPECT_EQ(a->resumes(), 0);
}

TEST(ClientPause, UnpauseSkipsAGoneLinkAndResumesTheOthers) {
  ClientPause p;
  auto a = std::make_shared<test::FakeLink>();
  p.Pause(ClientPause::Clock::now() + 10s, Mode::kWrite);
  ASSERT_TRUE(p.Park(std::make_shared<test::FakeLink>()));  // expired before the resume
  ASSERT_TRUE(p.Park(a));
  p.Unpause();
  EXPECT_EQ(a->resumes(), 1);
  EXPECT_EQ(p.parked(), 0u);
}

}  // namespace
}  // namespace tideskv
