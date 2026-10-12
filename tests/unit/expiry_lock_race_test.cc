// Regression for the final-review C2 finding: a write command must judge expiry with a
// timestamp taken after it holds its key locks, not before it started waiting for them.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "support/command_harness.h"
#include "types/errors.h"

namespace tideskv {
namespace {

using test::Bulk;
using test::CommandHarness;
using test::Err;
using test::Int;
using test::kNil;
using test::kOk;

struct ExpiryLockRace : ::testing::Test {
  CommandHarness h;

  // Runs `argv` on another thread while this test holds an Engine::Write on `held` (db 0).
  // Once the command has had time to block on the key lock, the clock moves `advance_ms`
  // forward and the lock is released. Returns the command's reply.
  std::string RunWhileLocked(const std::string& held, const ArgList& argv, int64_t advance_ms) {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::thread holder([&] {
      EXPECT_TRUE(h.services.engine_
                      .Write(0, {held},
                             [&](WriteTxn&, int64_t) {
                               entered = true;
                               while (!release) std::this_thread::yield();
                               return Status::Ok();
                             })
                      .ok());
    });
    while (!entered) std::this_thread::yield();
    std::string reply;
    std::thread client([&] { reply = h.Run(argv); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let it block on the lock
    h.clock().Advance(advance_ms);
    release = true;
    holder.join();
    client.join();
    return reply;
  }
};

TEST_F(ExpiryLockRace, PersistSeesKeyThatExpiredWhileWaiting) {
  EXPECT_EQ(h.Run({"SET", "k", "v", "PX", "100"}), kOk);
  EXPECT_EQ(RunWhileLocked("k", {"PERSIST", "k"}, 100), Int(0));
  EXPECT_EQ(h.Run({"EXISTS", "k"}), Int(0));
}

TEST_F(ExpiryLockRace, RenameOfKeyThatExpiredWhileWaitingKeepsDestination) {
  EXPECT_EQ(h.Run({"SET", "dst", "precious"}), kOk);
  EXPECT_EQ(h.Run({"SET", "k", "v", "PX", "100"}), kOk);
  EXPECT_EQ(RunWhileLocked("k", {"RENAME", "k", "dst"}, 100), Err(kErrNoSuchKey));
  EXPECT_EQ(h.Run({"GET", "dst"}), Bulk("precious"));
}

TEST_F(ExpiryLockRace, SetXxOnKeyThatExpiredWhileWaitingDoesNothing) {
  EXPECT_EQ(h.Run({"SET", "k", "old", "PX", "100"}), kOk);
  EXPECT_EQ(RunWhileLocked("k", {"SET", "k", "new", "XX"}, 100), kNil);
  EXPECT_EQ(h.Run({"GET", "k"}), kNil);
}

TEST_F(ExpiryLockRace, ExpireOnKeyThatExpiredWhileWaitingReturnsZero) {
  EXPECT_EQ(h.Run({"SET", "k", "v", "PX", "100"}), kOk);
  EXPECT_EQ(RunWhileLocked("k", {"EXPIRE", "k", "1000"}, 100), Int(0));
  EXPECT_EQ(h.Run({"EXISTS", "k"}), Int(0));
}

TEST_F(ExpiryLockRace, IncrAndAppendStartFreshOnKeyThatExpiredWhileWaiting) {
  EXPECT_EQ(h.Run({"SET", "n", "41", "PX", "100"}), kOk);
  EXPECT_EQ(RunWhileLocked("n", {"INCR", "n"}, 100), Int(1));
  EXPECT_EQ(h.Run({"SET", "s", "abc", "PX", "100"}), kOk);
  EXPECT_EQ(RunWhileLocked("s", {"APPEND", "s", "d"}, 100), Int(1));
}

}  // namespace
}  // namespace tideskv
