#include "types/slot_keys.h"

#include <gtest/gtest.h>

#include "support/command_harness.h"

namespace tideskv::test {
namespace {

bool Outside(CommandHarness& h) {
  bool out = false;
  EXPECT_TRUE(
      h.services.engine_.Read([&](ReadView& v, int64_t) { return MoveInto(HasKeysOutsideDb0(v), &out); })
          .ok());
  return out;
}

bool Empty(CommandHarness& h) {
  bool out = false;
  EXPECT_TRUE(
      h.services.engine_.Read([&](ReadView& v, int64_t) { return MoveInto(KeyspaceEmpty(v), &out); }).ok());
  return out;
}

TEST(SlotKeys, DbZeroKeysAreNotOutsideAndAnyKeyMakesTheKeyspaceNonEmpty) {
  CommandHarness h;
  EXPECT_TRUE(Empty(h));
  EXPECT_FALSE(Outside(h));
  ASSERT_EQ(h.Run({"SET", "k", "v"}), kOk);
  EXPECT_FALSE(Outside(h));
  EXPECT_FALSE(Empty(h));
  ASSERT_EQ(h.Run({"SELECT", "15"}), kOk);
  ASSERT_EQ(h.Run({"HSET", "h", "f", "v"}), Int(1));
  EXPECT_TRUE(Outside(h));
}

TEST(SlotKeys, CountAndListOneSlotOfDbZeroInKeyOrder) {
  CommandHarness h;
  for (const char* k : {"{b}2", "b", "{b}1", "foo"}) ASSERT_EQ(h.Run({"SET", k, "v"}), kOk);
  ASSERT_EQ(h.Run({"SELECT", "1"}), kOk);
  ASSERT_EQ(h.Run({"SET", "b", "other db"}), kOk);
  uint64_t n = 0;
  std::vector<std::string> keys;
  ASSERT_TRUE(h.services.engine_
                  .Read([&](ReadView& v, int64_t) {
                    if (Status st = MoveInto(CountKeysInSlot(v, 3300), &n); !st.ok()) return st;
                    return MoveInto(KeysInSlot(v, 3300, 2), &keys);
                  })
                  .ok());
  EXPECT_EQ(n, 3u);
  EXPECT_EQ(keys, (std::vector<std::string>{"b", "{b}1"}));
}

}  // namespace
}  // namespace tideskv::test
