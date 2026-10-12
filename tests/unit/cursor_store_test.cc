#include "types/cursor_store.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

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

TEST(CursorStore, MemoryIsBoundedByBytesAndNewestEntriesSurvive) {
  CursorStore cs;
  const std::string big(CursorStore::kMaxPositionBytes, 'p');
  std::vector<uint64_t> cursors;
  for (int i = 0; i < 300; ++i) {
    cursors.push_back(cs.Save(big));
    EXPECT_LE(cs.bytes(), CursorStore::kMaxBytes);
  }
  EXPECT_FALSE(cs.Load(cursors.front()).has_value());  // evicted to make room
  EXPECT_TRUE(cs.Load(cursors.back()) == big);         // EXPECT_TRUE: no 1 MiB failure dumps
  EXPECT_TRUE(cs.Load(cursors[cursors.size() - 2]).has_value());
  EXPECT_EQ(cs.Save("small"), cursors.back() + 1);
  EXPECT_EQ(cs.Load(cursors.back() + 1), "small");
}

TEST(CursorStore, OversizedPositionIsStoredAsAPrefix) {
  // A prefix sorts at or before the full position, so resuming from it can only repeat
  // keys (allowed by SCAN), never skip them.
  CursorStore cs;
  const std::string huge = std::string(CursorStore::kMaxPositionBytes, 'a') + "tail";
  const uint64_t c = cs.Save(huge);
  EXPECT_TRUE(cs.Load(c) == huge.substr(0, CursorStore::kMaxPositionBytes));
  EXPECT_LE(cs.bytes(), CursorStore::kMaxBytes);
}

}  // namespace
}  // namespace tideskv
