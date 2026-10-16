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

}  // namespace
}  // namespace tideskv::test
