#include "types/zset_pop.h"

#include <gtest/gtest.h>

#include <random>

#include "storage/memory_storage.h"
#include "types/errors.h"
#include "types/object_store.h"
#include "types/oid_allocator.h"

namespace tideskv {
namespace {

using Members = std::vector<std::string>;

Members Names(const std::vector<ScoreMember>& v) {
  Members out;
  for (const auto& p : v) out.push_back(p.member);
  return out;
}

struct ZsetPopTest : ::testing::Test {
  void SetUp() override {
    ASSERT_TRUE(oids.Init().ok());
    ASSERT_TRUE(Write([&](WriteTxn& t) {
                  return ZsetAdd(t, oids, 0, "z", {}, {{1, "a"}, {2, "b"}, {2, "c"}, {3, "d"}}, 0).status();
                }).ok());
  }
  template <typename Fn>
  Status Write(Fn fn) {
    auto txn = storage.BeginWrite();
    if (!txn.ok()) return txn.status();
    if (Status st = fn(**txn); !st.ok()) return st;
    return (*txn)->Commit();
  }
  InMemoryStorage storage;
  OidAllocator oids{storage};
  std::mt19937_64 rng{42};
};

TEST_F(ZsetPopTest, PopMinAndMaxOrder) {
  std::optional<std::vector<ScoreMember>> got;
  ASSERT_TRUE(Write([&](WriteTxn& t) { return MoveInto(ZsetPop(t, 0, "z", false, 2, 0), &got); }).ok());
  EXPECT_EQ(Names(*got), (Members{"a", "b"}));
  ASSERT_TRUE(Write([&](WriteTxn& t) { return MoveInto(ZsetPop(t, 0, "z", true, 5, 0), &got); }).ok());
  EXPECT_EQ(Names(*got), (Members{"d", "c"}));
  ASSERT_TRUE(Write([&](WriteTxn& t) { return MoveInto(ZsetPop(t, 0, "z", true, 1, 0), &got); }).ok());
  EXPECT_FALSE(got.has_value());  // the key went with its last member
  EXPECT_TRUE((*storage.Current())[CfIndex(ColumnFamily::kMeta)].empty());
}

TEST_F(ZsetPopTest, PopMaxTiesComeOutInDescendingMemberOrder) {
  std::optional<std::vector<ScoreMember>> got;
  ASSERT_TRUE(Write([&](WriteTxn& t) { return MoveInto(ZsetPop(t, 0, "z", true, 3, 0), &got); }).ok());
  EXPECT_EQ(Names(*got), (Members{"d", "c", "b"}));
}

TEST_F(ZsetPopTest, PopFirstSkipsMissingAndStopsAtWrongType) {
  ASSERT_TRUE(Write([&](WriteTxn& t) { return WriteString(t, 0, "s", "v", 0, StoredObject{}); }).ok());
  std::optional<ZPoppedFrom> got;
  ASSERT_TRUE(Write([&](WriteTxn& t) {
                return MoveInto(ZsetPopFirst(t, 0, {"x", "z", "s"}, false, 1, 0), &got);
              }).ok());
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->key, "z");
  EXPECT_EQ(
      Write([&](WriteTxn& t) { return ZsetPopFirst(t, 0, {"x", "s", "z"}, false, 1, 0).status(); }).message(),
      kErrWrongType);
  ASSERT_TRUE(
      Write([&](WriteTxn& t) { return MoveInto(ZsetPopFirst(t, 0, {"x", "y"}, false, 1, 0), &got); }).ok());
  EXPECT_FALSE(got.has_value());
}

TEST_F(ZsetPopTest, RemoveRangeByRankScoreLex) {
  int64_t n = 0;
  RangeSpec rank;
  rank.start = 0;
  rank.stop = 0;
  ASSERT_TRUE(Write([&](WriteTxn& t) { return MoveInto(ZsetRemoveRange(t, 0, "z", rank, 0), &n); }).ok());
  EXPECT_EQ(n, 1);
  RangeSpec score;
  score.by = RangeBy::kScore;
  ASSERT_TRUE(ParseScoreRange("(1", "2", &score.score).ok());
  ASSERT_TRUE(Write([&](WriteTxn& t) { return MoveInto(ZsetRemoveRange(t, 0, "z", score, 0), &n); }).ok());
  EXPECT_EQ(n, 2);
  RangeSpec lex;
  lex.by = RangeBy::kLex;
  ASSERT_TRUE(ParseLexRange("-", "+", &lex.lex).ok());
  ASSERT_TRUE(Write([&](WriteTxn& t) { return MoveInto(ZsetRemoveRange(t, 0, "z", lex, 0), &n); }).ok());
  EXPECT_EQ(n, 1);
  EXPECT_TRUE((*storage.Current())[CfIndex(ColumnFamily::kZScore)].empty());
}

TEST_F(ZsetPopTest, RandomMembers) {
  auto view = storage.BeginRead();
  EXPECT_EQ(Names(*ZsetRandomMembers(**view, 0, "z", 10, rng, 0)),
            (Members{"d", "c", "b", "a"}));  // whole set, desc
  EXPECT_EQ(ZsetRandomMembers(**view, 0, "z", 2, rng, 0)->size(), 2u);
  EXPECT_EQ(ZsetRandomMembers(**view, 0, "z", -7, rng, 0)->size(), 7u);  // repeats allowed
  EXPECT_TRUE(ZsetRandomMembers(**view, 0, "nokey", 3, rng, 0)->empty());
  EXPECT_TRUE(ZsetRandomMember(**view, 0, "z", rng, 0)->has_value());
  EXPECT_FALSE(ZsetRandomMember(**view, 0, "nokey", rng, 0)->has_value());
}

}  // namespace
}  // namespace tideskv
