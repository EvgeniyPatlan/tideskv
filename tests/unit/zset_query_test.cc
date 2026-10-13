#include "types/zset_query.h"

#include <gtest/gtest.h>

#include "storage/memory_storage.h"
#include "types/object_store.h"
#include "types/oid_allocator.h"
#include "types/zset_type.h"

namespace tideskv {
namespace {

using Members = std::vector<std::string>;

struct ZsetQueryTest : ::testing::Test {
  void SetUp() override {
    ASSERT_TRUE(oids.Init().ok());
    Seed("z", {{1, "a"}, {2, "b"}, {2, "c"}, {3, "d"}, {4, "e"}});
  }
  template <typename Fn>
  Status Write(Fn fn) {
    auto txn = storage.BeginWrite();
    if (!txn.ok()) return txn.status();
    if (Status st = fn(**txn); !st.ok()) return st;
    return (*txn)->Commit();
  }
  void Seed(std::string_view key, std::vector<ScoreMember> pairs) {
    ASSERT_TRUE(Write([&](WriteTxn& t) { return ZsetAdd(t, oids, 0, key, {}, pairs, 0).status(); }).ok());
  }
  Members Range(const RangeSpec& spec, std::string_view key = "z") {
    auto view = storage.BeginRead();
    auto r = ZsetRange(**view, 0, key, spec, 0);
    EXPECT_TRUE(r.ok()) << r.status().ToString();
    Members out;
    if (r.ok()) {
      for (const auto& p : *r) out.push_back(p.member);
    }
    return out;
  }
  static RangeSpec Rank(int64_t start, int64_t stop, bool rev = false) {
    RangeSpec s;
    s.start = start;
    s.stop = stop;
    s.reverse = rev;
    return s;
  }
  static RangeSpec Score(std::string_view min, std::string_view max, bool rev = false, int64_t off = 0,
                         int64_t count = -1) {
    RangeSpec s;
    s.by = RangeBy::kScore;
    EXPECT_TRUE(ParseScoreRange(min, max, &s.score).ok());
    s.reverse = rev;
    s.offset = off;
    s.count = count;
    return s;
  }

  InMemoryStorage storage;
  OidAllocator oids{storage};
};

TEST_F(ZsetQueryTest, ByRank) {
  EXPECT_EQ(Range(Rank(0, -1)), (Members{"a", "b", "c", "d", "e"}));
  EXPECT_EQ(Range(Rank(1, 2)), (Members{"b", "c"}));
  EXPECT_EQ(Range(Rank(0, 1, true)), (Members{"e", "d"}));  // index 0 = highest
  EXPECT_EQ(Range(Rank(-2, -1, true)), (Members{"b", "a"}));
  EXPECT_EQ(Range(Rank(3, 1)), Members{});
  EXPECT_EQ(Range(Rank(0, -1), "nokey"), Members{});
}

TEST_F(ZsetQueryTest, ByScoreWithLimit) {
  EXPECT_EQ(Range(Score("2", "3")), (Members{"b", "c", "d"}));
  EXPECT_EQ(Range(Score("(2", "+inf")), (Members{"d", "e"}));
  EXPECT_EQ(Range(Score("1", "3", true)), (Members{"d", "c", "b", "a"}));
  EXPECT_EQ(Range(Score("-inf", "+inf", false, 1, 2)), (Members{"b", "c"}));
  EXPECT_EQ(Range(Score("-inf", "+inf", false, 1, 0)), Members{});
  EXPECT_EQ(Range(Score("-inf", "+inf", false, -1, 2)), Members{});  // negative offset: empty
  EXPECT_EQ(Range(Score("-inf", "+inf", false, 9, -1)), Members{});
}

TEST_F(ZsetQueryTest, ByLex) {
  Seed("l", {{0, "a"}, {0, "b"}, {0, "c"}, {0, "d"}});
  RangeSpec s;
  s.by = RangeBy::kLex;
  ASSERT_TRUE(ParseLexRange("[b", "(d", &s.lex).ok());
  EXPECT_EQ(Range(s, "l"), (Members{"b", "c"}));
  s.reverse = true;
  EXPECT_EQ(Range(s, "l"), (Members{"c", "b"}));
  s.reverse = false;
  s.offset = 1;
  s.count = 5;
  EXPECT_EQ(Range(s, "l"), (Members{"c"}));
}

TEST_F(ZsetQueryTest, ReverseScoreWithExclusiveMaxOverTies) {
  // b and c tie at 2: an exclusive max of 2 must skip both, an inclusive one keeps both (c before b).
  EXPECT_EQ(Range(Score("1", "(2", true)), (Members{"a"}));
  EXPECT_EQ(Range(Score("(1", "(3", true)), (Members{"c", "b"}));
  EXPECT_EQ(Range(Score("1", "2", true)), (Members{"c", "b", "a"}));
  EXPECT_EQ(Range(Score("(2", "(+inf", true)), (Members{"e", "d"}));
  EXPECT_EQ(Range(Score("(-inf", "(1", true)), Members{});
  EXPECT_EQ(Range(Score("(5", "+inf", true)), Members{});
  EXPECT_EQ(Range(Score("-inf", "2", true, 1, 2)), (Members{"b", "a"}));
}

TEST_F(ZsetQueryTest, ReverseLexEndingBeforePlus) {
  Seed("l", {{0, "a"}, {0, "b"}, {0, "c"}, {0, "d"}, {0, "e"}});
  RangeSpec s;
  s.by = RangeBy::kLex;
  s.reverse = true;
  ASSERT_TRUE(ParseLexRange("[b", "[d", &s.lex).ok());  // max is below the last member "e"
  EXPECT_EQ(Range(s, "l"), (Members{"d", "c", "b"}));
  ASSERT_TRUE(ParseLexRange("-", "(c", &s.lex).ok());
  EXPECT_EQ(Range(s, "l"), (Members{"b", "a"}));
  ASSERT_TRUE(ParseLexRange("(e", "+", &s.lex).ok());  // empty first hit
  EXPECT_EQ(Range(s, "l"), Members{});
  ASSERT_TRUE(ParseLexRange("(a", "(b", &s.lex).ok());
  EXPECT_EQ(Range(s, "l"), Members{});
  ASSERT_TRUE(ParseLexRange("[c", "[c", &s.lex).ok());
  EXPECT_EQ(Range(s, "l"), (Members{"c"}));
  s.reverse = false;
  ASSERT_TRUE(ParseLexRange("[b", "[d", &s.lex).ok());
  EXPECT_EQ(Range(s, "l"), (Members{"b", "c", "d"}));
}

TEST_F(ZsetQueryTest, Rank) {
  auto view = storage.BeginRead();
  auto r = ZsetRank(**view, 0, "z", "c", false, 0);
  ASSERT_TRUE(r.ok() && r->has_value());
  EXPECT_EQ((*r)->rank, 2);
  EXPECT_EQ((*r)->score, 2);
  EXPECT_EQ((*ZsetRank(**view, 0, "z", "c", true, 0))->rank, 2);
  EXPECT_EQ((*ZsetRank(**view, 0, "z", "e", true, 0))->rank, 0);
  EXPECT_FALSE(ZsetRank(**view, 0, "z", "zz", false, 0)->has_value());
  EXPECT_FALSE(ZsetRank(**view, 0, "nokey", "a", false, 0)->has_value());
}

TEST_F(ZsetQueryTest, RangeStoreReplacesDstAndDeletesItWhenEmpty) {
  // dst starts as a string with a deadline: the store replaces it and drops the TTL.
  ASSERT_TRUE(Write([&](WriteTxn& t) {
                return WriteString(t, 0, "dst", "v", 9'999'999'999'999, StoredObject{});
              }).ok());
  int64_t n = 0;
  ASSERT_TRUE(Write([&](WriteTxn& t) {
                return MoveInto(ZsetRangeStore(t, oids, 0, "dst", "z", Rank(0, 1), 0), &n);
              }).ok());
  EXPECT_EQ(n, 2);
  EXPECT_EQ(Range(Rank(0, -1), "dst"), (Members{"a", "b"}));
  {
    auto view = storage.BeginRead();
    auto obj = LoadObject(**view, 0, "dst", 0);
    ASSERT_TRUE(obj.ok() && obj->live());
    EXPECT_EQ(obj->meta.type, encoding::ValueType::kZSet);
    EXPECT_EQ(obj->meta.expire_ms, 0);
    EXPECT_TRUE((*storage.Current())[CfIndex(ColumnFamily::kExpire)].empty());
  }
  ASSERT_TRUE(Write([&](WriteTxn& t) {
                return MoveInto(ZsetRangeStore(t, oids, 0, "dst", "z", Rank(9, 10), 0), &n);
              }).ok());
  EXPECT_EQ(n, 0);
  auto view = storage.BeginRead();
  EXPECT_FALSE(LoadObject(**view, 0, "dst", 0)->live());
}

}  // namespace
}  // namespace tideskv
