#include "server/blocking.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "server/connection_block_handler.h"
#include "support/fake_link.h"

namespace tideskv {
namespace {

using encoding::ValueType;

std::shared_ptr<BlockedClient> MakeClient(BlockingRegistry& r, uint64_t id, std::vector<std::string> keys,
                                          const std::shared_ptr<test::FakeLink>& link, uint8_t db = 0) {
  Session s;
  s.id = id;
  s.db = db;
  s.link = link;
  return std::make_shared<BlockedClient>(r, s, std::vector<std::string>{"blpop"}, ValueType::kList,
                                         std::move(keys), std::nullopt);
}

TEST(BlockingRegistry, AddFindRemoveAndCounters) {
  BlockingRegistry r;
  auto link = std::make_shared<test::FakeLink>();
  auto a = MakeClient(r, 1, {"k1", "k2", "k1"}, link);  // duplicate key collapsed
  EXPECT_EQ(a->keys(), (std::vector<std::string>{"k1", "k2"}));
  EXPECT_FALSE(r.HasWaiters(0, "k1"));
  r.Add(a);
  EXPECT_TRUE(r.HasWaiters(0, "k1"));
  EXPECT_TRUE(r.HasWaiters(0, "k2"));
  EXPECT_FALSE(r.HasWaiters(1, "k1"));  // other db
  EXPECT_EQ(r.Find(1), a);
  EXPECT_EQ(r.blocked_clients(), 1u);
  EXPECT_EQ(r.blocking_keys(), 2u);
  r.Remove(*a);
  r.Remove(*a);  // idempotent
  EXPECT_FALSE(r.HasWaiters(0, "k1"));
  EXPECT_EQ(r.Find(1), nullptr);
  EXPECT_EQ(r.blocked_clients(), 0u);
  EXPECT_EQ(r.blocking_keys(), 0u);
}

TEST(BlockingRegistry, AddingADuplicateClientIdReplacesTheOldEntry) {
  BlockingRegistry r;
  auto link = std::make_shared<test::FakeLink>();
  auto old_wait = MakeClient(r, 1, {"k1", "k2"}, link), new_wait = MakeClient(r, 1, {"k2", "k3"}, link);
  r.Add(old_wait);
  r.Add(new_wait);
  EXPECT_EQ(r.Find(1), new_wait);
  EXPECT_EQ(r.blocked_clients(), 1u);
  EXPECT_FALSE(r.HasWaiters(0, "k1"));  // nothing of the old entry is left behind
  EXPECT_EQ(r.WaitersOn(0, "k2"), (std::vector<std::shared_ptr<BlockedClient>>{new_wait}));
  EXPECT_EQ(r.blocking_keys(), 2u);
  r.Remove(*old_wait);  // no longer registered: a no-op
  EXPECT_EQ(r.Find(1), new_wait);
  r.Remove(*new_wait);
  EXPECT_EQ(r.blocked_clients(), 0u);
  EXPECT_EQ(r.blocking_keys(), 0u);
}

TEST(BlockingRegistry, WaitersAreFifoPerKey) {
  BlockingRegistry r;
  auto link = std::make_shared<test::FakeLink>();
  auto a = MakeClient(r, 1, {"k"}, link), b = MakeClient(r, 2, {"x", "k"}, link),
       c = MakeClient(r, 3, {"k"}, link);
  r.Add(a);
  r.Add(b);
  r.Add(c);
  const auto waiters = r.WaitersOn(0, "k");
  ASSERT_EQ(waiters.size(), 3u);
  EXPECT_EQ(waiters[0], a);
  EXPECT_EQ(waiters[1], b);
  EXPECT_EQ(waiters[2], c);
  EXPECT_TRUE(r.WaitersOn(0, "nope").empty());
}

TEST(BlockedClient, ServeCancelUnblockEndTheWaitOnce) {
  BlockingRegistry r;
  auto link = std::make_shared<test::FakeLink>();
  auto a = MakeClient(r, 1, {"k"}, link);
  r.Add(a);
  EXPECT_EQ(a->TryServe([] { return std::nullopt; }), BlockedClient::ServeResult::kStillBlocked);
  EXPECT_TRUE(r.HasWaiters(0, "k"));
  EXPECT_EQ(a->TryServe([] { return std::optional<std::string>(":1\r\n"); }),
            BlockedClient::ServeResult::kServed);
  EXPECT_EQ(link->Take(), (std::vector<std::string>{":1\r\n"}));
  EXPECT_FALSE(r.HasWaiters(0, "k"));  // ending the wait removes it from the registry
  EXPECT_FALSE(a->Cancel());
  EXPECT_FALSE(a->Unblock("-UNBLOCKED x\r\n"));
  EXPECT_EQ(a->TryServe([] { return std::optional<std::string>("x"); }), BlockedClient::ServeResult::kGone);
  EXPECT_TRUE(link->Take().empty());

  auto b = MakeClient(r, 2, {"k"}, link);
  r.Add(b);
  EXPECT_TRUE(b->Unblock("*-1\r\n"));
  EXPECT_EQ(link->Take(), (std::vector<std::string>{"*-1\r\n"}));
  auto c = MakeClient(r, 3, {"k"}, link);
  r.Add(c);
  EXPECT_TRUE(c->Cancel());  // timeout/disconnect: the caller replies itself, nothing is delivered
  EXPECT_TRUE(link->Take().empty());
  EXPECT_EQ(r.blocked_clients(), 0u);
}

// Review Focus #1: a timeout racing a serve ends the wait exactly once.
TEST(BlockedClient, ServeRacingCancelHasOneWinner) {
  for (int round = 0; round < 200; ++round) {
    BlockingRegistry r;
    auto link = std::make_shared<test::FakeLink>();
    auto a = MakeClient(r, 1, {"k"}, link);
    r.Add(a);
    std::atomic<bool> go{false};
    std::atomic<int> attempts{0};
    BlockedClient::ServeResult served{};
    bool cancelled = false;
    std::thread server([&] {
      while (!go) {
      }
      served = a->TryServe([&] {
        ++attempts;
        return std::optional<std::string>("v");
      });
    });
    std::thread timer([&] {
      while (!go) {
      }
      cancelled = a->Cancel();
    });
    go = true;
    server.join();
    timer.join();
    const bool won_serve = served == BlockedClient::ServeResult::kServed;
    EXPECT_NE(won_serve, cancelled);
    EXPECT_EQ(attempts.load(), won_serve ? 1 : 0);  // a cancelled waiter never runs its command
    EXPECT_EQ(link->Take().size(), won_serve ? 1u : 0u);
    EXPECT_EQ(r.blocked_clients(), 0u);
  }
}

// A timeout beyond what steady_clock can represent (BLPOP k 10000000000) blocks forever, as in Redis.
TEST(ConnectionBlockHandler, HugeTimeoutBlocksForeverAndNormalOneHasADeadline) {
  BlockingRegistry r;
  Session s;
  s.id = 1;
  ReadyKeys ready;
  const std::vector<std::string> argv{"blpop", "k", "10000000000"};
  ConnectionBlockHandler handler(r, s, argv, ready);
  handler.Block({ValueType::kList, {"k"}, 10'000'000'000'000});
  ASSERT_NE(s.blocked, nullptr);
  EXPECT_EQ(s.blocked->deadline(), std::nullopt);
  EXPECT_TRUE(s.blocked->Cancel());

  Session s2;
  s2.id = 2;
  ConnectionBlockHandler handler2(r, s2, argv, ready);
  const auto before = BlockedClient::Clock::now();
  handler2.Block({ValueType::kList, {"k"}, 200});
  ASSERT_NE(s2.blocked, nullptr);
  ASSERT_TRUE(s2.blocked->deadline().has_value());
  EXPECT_GE(*s2.blocked->deadline(), before + std::chrono::milliseconds(200));
  EXPECT_TRUE(s2.blocked->Cancel());
}

}  // namespace
}  // namespace tideskv
