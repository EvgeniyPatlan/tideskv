#include "types/cursor_store.h"

#include <gtest/gtest.h>

namespace tideskv {
namespace {

TEST(CursorStore, SaveLoadRoundTrip) {
  CursorStore cs;
  const uint64_t c1 = cs.Save("pos-1");
  const uint64_t c2 = cs.Save("pos-2");
  EXPECT_NE(c1, 0u);
  EXPECT_NE(c1, c2);
  EXPECT_EQ(cs.Load(c1), "pos-1");
  EXPECT_EQ(cs.Load(c2), "pos-2");
  EXPECT_EQ(cs.Load(0), std::nullopt);
  EXPECT_EQ(cs.Load(c2 + 1000), std::nullopt);
}

TEST(CursorStore, OldCursorsAreEvicted) {
  CursorStore cs;
  const uint64_t first = cs.Save("first");
  for (size_t i = 0; i < CursorStore::kCapacity; ++i) cs.Save("x");
  EXPECT_EQ(cs.Load(first), std::nullopt);
}

}  // namespace
}  // namespace tideskv
