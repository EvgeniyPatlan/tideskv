#include "server/blocking.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

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

}  // namespace
}  // namespace tideskv
