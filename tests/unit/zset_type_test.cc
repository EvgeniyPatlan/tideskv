#include "types/zset_type.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "encoding/key_codec.h"
#include "encoding/meta_codec.h"
#include "storage/memory_storage.h"
#include "types/errors.h"
#include "types/oid_allocator.h"

namespace tideskv {
namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

struct ZsetTypeTest : ::testing::Test {
  void SetUp() override { ASSERT_TRUE(oids.Init().ok()); }
  template <typename Fn>
  Status Write(Fn fn) {
    auto txn = storage.BeginWrite();
    if (!txn.ok()) return txn.status();
    if (Status st = fn(**txn); !st.ok()) return st;
    return (*txn)->Commit();
  }
  ZAddResult Add(std::vector<ScoreMember> pairs, ZAddFlags f = {}) {
    ZAddResult r;
    const Status st = Write([&](WriteTxn& t) { return MoveInto(ZsetAdd(t, oids, 0, "z", f, pairs, 0), &r); });
    EXPECT_TRUE(st.ok()) << st.ToString();
    return r;
  }
  std::vector<std::pair<double, std::string>> All() {
    auto view = storage.BeginRead();
    auto c = LoadCollection(**view, 0, "z", encoding::ValueType::kZSet, 0);
    std::vector<std::pair<double, std::string>> out;
    EXPECT_TRUE(ForEachByScore(**view, *c, false, [&](double s, std::string_view m) -> Result<bool> {
                  out.emplace_back(s, std::string(m));
                  return true;
                }).ok());
    return out;
  }
  std::optional<double> Score(std::string_view m) {
    auto view = storage.BeginRead();
    return *ZsetScore(**view, 0, "z", m, 0);
  }

  InMemoryStorage storage;
  OidAllocator oids{storage};
};

TEST_F(ZsetTypeTest, AddOrdersByScoreThenMember) {
  EXPECT_EQ(Add({{2, "b"}, {1, "c"}, {1, "a"}, {-kInf, "x"}}).added, 4);
  EXPECT_EQ(All(), (std::vector<std::pair<double, std::string>>{{-kInf, "x"}, {1, "a"}, {1, "c"}, {2, "b"}}));
  auto view = storage.BeginRead();
  EXPECT_EQ(*ZsetCard(**view, 0, "z", 0), 4);
}

TEST_F(ZsetTypeTest, UpdateMovesTheOrderEntry) {
  Add({{1, "a"}, {2, "b"}});
  const ZAddResult r = Add({{3, "a"}, {2, "b"}}, {.ch = true});
  EXPECT_EQ(r.added, 0);
  EXPECT_EQ(r.updated, 1);  // b's score did not change
  EXPECT_EQ(All(), (std::vector<std::pair<double, std::string>>{{2, "b"}, {3, "a"}}));
  EXPECT_EQ((*storage.Current())[CfIndex(ColumnFamily::kZScore)].size(), 2u);  // old entry gone
}

TEST_F(ZsetTypeTest, NxXxGtLt) {
  Add({{5, "a"}});
  EXPECT_EQ(Add({{1, "a"}, {1, "b"}}, {.nx = true}).added, 1);
  EXPECT_EQ(Score("a"), 5);
  EXPECT_EQ(Add({{1, "c"}}, {.xx = true}).added, 0);
  EXPECT_EQ(Score("c"), std::nullopt);
  EXPECT_EQ(Add({{4, "a"}}, {.gt = true, .ch = true}).updated, 0);
  EXPECT_EQ(Add({{6, "a"}}, {.gt = true, .ch = true}).updated, 1);
  EXPECT_EQ(Add({{7, "a"}}, {.lt = true, .ch = true}).updated, 0);
  EXPECT_EQ(Add({{7, "new"}}, {.lt = true}).added, 1);  // GT/LT never block a new member
}

TEST_F(ZsetTypeTest, IncrRepliesTheScoreOrNothing) {
  EXPECT_EQ(Add({{2, "a"}}, {.incr = true}).incr_score, 2.0);  // creates the member
  EXPECT_EQ(Add({{0.5, "a"}}, {.incr = true}).incr_score, 2.5);
  EXPECT_EQ(Add({{1, "a"}}, {.nx = true, .incr = true}).incr_score, std::nullopt);
  EXPECT_EQ(Add({{-1, "a"}}, {.gt = true, .incr = true}).incr_score, std::nullopt);
  EXPECT_EQ(Add({{1, "zz"}}, {.xx = true, .incr = true}).incr_score, std::nullopt);
}

TEST_F(ZsetTypeTest, IncrToNanFails) {
  Add({{kInf, "a"}});
  EXPECT_EQ(Write([&](WriteTxn& t) {
              return ZsetAdd(t, oids, 0, "z", {.incr = true}, {{-kInf, "a"}}, 0).status();
            }).message(),
            kErrScoreNan);
  EXPECT_EQ(Score("a"), kInf);
}

TEST_F(ZsetTypeTest, MinusZeroIsStoredAsZero) {  // F2
  Add({{-0.0, "a"}});
  ASSERT_TRUE(Score("a").has_value());
  EXPECT_FALSE(std::signbit(*Score("a")));
}

TEST_F(ZsetTypeTest, XxOnMissingKeyCreatesNothing) {
  Add({{1, "a"}}, {.xx = true});
  EXPECT_TRUE((*storage.Current())[CfIndex(ColumnFamily::kMeta)].empty());
}

TEST_F(ZsetTypeTest, RemoveToEmptyDeletesEverything) {
  Add({{1, "a"}, {2, "b"}});
  int64_t n = 0;
  ASSERT_TRUE(
      Write([&](WriteTxn& t) { return MoveInto(ZsetRemove(t, 0, "z", {"a", "a", "zz", "b"}, 0), &n); }).ok());
  EXPECT_EQ(n, 2);
  const auto tables = storage.Current();
  for (auto cf : {ColumnFamily::kMeta, ColumnFamily::kSubkey, ColumnFamily::kZScore})
    EXPECT_TRUE((*tables)[CfIndex(cf)].empty());
}

TEST_F(ZsetTypeTest, CountAndLexCount) {
  Add({{1, "a"}, {2, "b"}, {2, "c"}, {3, "d"}});
  auto view = storage.BeginRead();
  ScoreRange sr;
  ASSERT_TRUE(ParseScoreRange("(1", "2", &sr).ok());
  EXPECT_EQ(*ZsetCount(**view, 0, "z", sr, 0), 2);
  ASSERT_TRUE(ParseScoreRange("-inf", "+inf", &sr).ok());
  EXPECT_EQ(*ZsetCount(**view, 0, "z", sr, 0), 4);
  ASSERT_TRUE(ParseScoreRange("3", "(3", &sr).ok());
  EXPECT_EQ(*ZsetCount(**view, 0, "z", sr, 0), 0);
  Add({{0, "p"}, {0, "q"}, {0, "r"}});  // lex commands assume equal scores; p q r sort first at score 0
  view = storage.BeginRead();
  LexRange lr;
  ASSERT_TRUE(ParseLexRange("[q", "+", &lr).ok());
  // q, r, a, b, c, d: from the first element >= "q" in rank order (mixed scores: Redis's linear semantic).
  EXPECT_EQ(*ZsetLexCount(**view, 0, "z", lr, 0), 6);
}

TEST_F(ZsetTypeTest, ReverseRangeScans) {
  Add({{1, "a"}, {2, "b"}, {2, "c"}, {3, "d"}});
  auto view = storage.BeginRead();
  auto c = LoadCollection(**view, 0, "z", encoding::ValueType::kZSet, 0);
  ScoreRange sr;
  ASSERT_TRUE(ParseScoreRange("1", "(3", &sr).ok());
  std::vector<std::string> seen;
  ASSERT_TRUE(ForEachInScoreRange(**view, *c, sr, true, [&](double, std::string_view m) -> Result<bool> {
                seen.emplace_back(m);
                return true;
              }).ok());
  EXPECT_EQ(seen, (std::vector<std::string>{"c", "b", "a"}));
}

TEST_F(ZsetTypeTest, WrongType) {
  ASSERT_TRUE(Write([](WriteTxn& t) {
                return t.Put(ColumnFamily::kMeta, encoding::MetaKeyFor(0, "z"),
                             encoding::EncodeMetadata(encoding::Metadata{}, "str"));
              }).ok());
  auto view = storage.BeginRead();
  EXPECT_EQ(ZsetCard(**view, 0, "z", 0).status().message(), kErrWrongType);
}

}  // namespace
}  // namespace tideskv
