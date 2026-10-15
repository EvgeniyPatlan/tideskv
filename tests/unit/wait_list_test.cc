#include "repl/wait_list.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "server/blocking.h"

namespace tideskv::repl {
namespace {

std::shared_ptr<BlockedClient> Client(BlockingRegistry& reg, uint64_t id) {
  Session s;
  s.id = id;
  auto c =
      std::make_shared<BlockedClient>(reg, s, std::vector<std::string>{"WAIT"}, encoding::ValueType::kList,
                                      std::vector<std::string>{}, std::nullopt);
  reg.Add(c);
  return c;
}

TEST(WaitList, CountAtLeast) {
  EXPECT_EQ(CountAtLeast({}, 0), 0);
  EXPECT_EQ(CountAtLeast({3, 5, 9}, 5), 2);
  EXPECT_EQ(CountAtLeast({3, 5, 9}, 0), 3);
}

TEST(WaitList, ReleasesOnlyTheWaitersTheAcksSatisfy) {
  BlockingRegistry reg;
  WaitList w;
  auto a = Client(reg, 1);
  auto b = Client(reg, 2);
  w.Add(a, 10, 1);
  w.Add(b, 20, 2);
  EXPECT_TRUE(w.Release({9, 9}).empty());  // PF4: no ACK reaches 10
  auto ready = w.Release({12, 15});
  ASSERT_EQ(ready.size(), 1u);
  EXPECT_EQ(ready[0].client, a);
  EXPECT_EQ(ready[0].count, 2);  // the count can exceed numreplicas
  EXPECT_EQ(w.size(), 1u);
  ready = w.Release({20, 25});
  ASSERT_EQ(ready.size(), 1u);
  EXPECT_EQ(ready[0].client, b);
  EXPECT_EQ(w.size(), 0u);
}

TEST(WaitList, RepeatedAcksReleaseAWaiterOnce) {  // CF-M6b-7: on_ack fires on every valid ACK
  BlockingRegistry reg;
  WaitList w;
  auto a = Client(reg, 1);
  w.Add(a, 5, 1);
  EXPECT_EQ(w.Release({5}).size(), 1u);
  EXPECT_TRUE(w.Release({5}).empty());
  EXPECT_TRUE(w.Release({6}).empty());
}

TEST(WaitList, PruneDropsEndedWaits) {
  BlockingRegistry reg;
  WaitList w;
  auto a = Client(reg, 1);
  auto b = Client(reg, 2);
  w.Add(a, 10, 1);
  w.Add(b, 10, 1);
  ASSERT_TRUE(a->Cancel());  // a timeout
  w.Prune();
  EXPECT_EQ(w.size(), 1u);  // b still waits
  auto ready = w.Release({10});
  ASSERT_EQ(ready.size(), 1u);
  EXPECT_EQ(ready[0].client, b);
}

}  // namespace
}  // namespace tideskv::repl
