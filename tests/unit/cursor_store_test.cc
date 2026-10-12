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
  ASSERT_TRUE(cs.Load(c1).has_value());
  ASSERT_TRUE(cs.Load(c2).has_value());
  EXPECT_EQ(cs.Load(c1)->key, "pos-1");
  EXPECT_EQ(cs.Load(c2)->key, "pos-2");
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
  ASSERT_TRUE(cs.Load(cursors.back()).has_value());
  EXPECT_TRUE(cs.Load(cursors.back())->key == big);  // EXPECT_TRUE: no 1 MiB failure dumps
  EXPECT_TRUE(cs.Load(cursors[cursors.size() - 2]).has_value());
  EXPECT_EQ(cs.Save("small"), cursors.back() + 1);
  ASSERT_TRUE(cs.Load(cursors.back() + 1).has_value());
  EXPECT_EQ(cs.Load(cursors.back() + 1)->key, "small");
}

TEST(CursorStore, UntruncatedPositionIsStoredWhole) {
  CursorStore cs;
  const std::string exact(CursorStore::kMaxPositionBytes, 'e');  // at the limit: not truncated
  const auto small = cs.Load(cs.Save("pos"));
  const auto at_limit = cs.Load(cs.Save(exact));
  ASSERT_TRUE(small.has_value() && at_limit.has_value());
  EXPECT_FALSE(small->truncated);
  EXPECT_EQ(small->key, "pos");
  EXPECT_EQ(small->full_size, 3u);
  EXPECT_FALSE(at_limit->truncated);
  EXPECT_TRUE(at_limit->key == exact);
  EXPECT_EQ(at_limit->full_size, exact.size());
}

TEST(CursorStore, OversizedPositionIsStoredAsPrefixSizeAndDigest) {
  // The prefix sorts at or before the full position; size + digest identify the full
  // position among the keys sharing the prefix, so a resume can step past it.
  CursorStore cs;
  const std::string huge = std::string(CursorStore::kMaxPositionBytes, 'a') + "tail";
  const auto pos = cs.Load(cs.Save(huge));
  ASSERT_TRUE(pos.has_value());
  EXPECT_TRUE(pos->truncated);
  EXPECT_TRUE(pos->key == huge.substr(0, CursorStore::kMaxPositionBytes));
  EXPECT_EQ(pos->full_size, huge.size());
  EXPECT_EQ(pos->digest, PositionDigest(huge));
  EXPECT_LE(cs.bytes(), CursorStore::kMaxBytes);
}

TEST(CursorStore, DigestDistinguishesPositionsSharingThePrefix) {
  const std::string prefix(CursorStore::kMaxPositionBytes, 'a');
  EXPECT_EQ(PositionDigest(prefix + "1"), PositionDigest(prefix + "1"));
  EXPECT_NE(PositionDigest(prefix + "1"), PositionDigest(prefix + "2"));
}

TEST(CursorStore, TruncatedPositionsStayWithinTheByteBound) {
  CursorStore cs;
  const std::string huge = std::string(CursorStore::kMaxPositionBytes, 'h') + std::string(4096, 't');
  std::vector<uint64_t> cursors;
  for (int i = 0; i < 100; ++i) {
    cursors.push_back(cs.Save(huge));
    EXPECT_LE(cs.bytes(), CursorStore::kMaxBytes);
  }
  EXPECT_FALSE(cs.Load(cursors.front()).has_value());
  EXPECT_TRUE(cs.Load(cursors.back()).has_value());
}

}  // namespace
}  // namespace tideskv
