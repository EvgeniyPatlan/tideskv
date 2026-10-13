#include "types/list_edit.h"

#include <gtest/gtest.h>

#include "storage/memory_storage.h"
#include "types/errors.h"
#include "types/list_type.h"
#include "types/oid_allocator.h"

namespace tideskv {
namespace {

using Strings = std::vector<std::string>;
using Positions = std::vector<int64_t>;

struct ListEditTest : ::testing::Test {
  void SetUp() override { ASSERT_TRUE(oids.Init().ok()); }

  template <typename Fn>
  Status Write(Fn fn) {
    auto txn = storage.BeginWrite();
    if (!txn.ok()) return txn.status();
    if (Status st = fn(**txn); !st.ok()) return st;
    return (*txn)->Commit();
  }
  void Fill(std::vector<std::string_view> values) {
    ASSERT_TRUE(Write([&](WriteTxn& t) {
                  return ListPush(t, oids, 0, "l", ListEnd::kRight, values, false, 0).status();
                }).ok());
  }
  Strings All() {
    auto view = storage.BeginRead();
    return *ListRange(**view, 0, "l", 0, -1, 0);
  }
  int64_t Insert(InsertWhere where, std::string_view pivot, std::string_view value) {
    int64_t n = 0;
    EXPECT_TRUE(Write([&](WriteTxn& t) { return MoveInto(ListInsert(t, 0, "l", where, pivot, value, 0), &n); }).ok());
    return n;
  }
  int64_t Remove(int64_t count, std::string_view value) {
    int64_t n = 0;
    EXPECT_TRUE(Write([&](WriteTxn& t) { return MoveInto(ListRemove(t, 0, "l", count, value, 0), &n); }).ok());
    return n;
  }
  std::optional<Positions> Pos(std::string_view value, ListPosOptions o) {
    auto view = storage.BeginRead();
    auto r = ListPos(**view, 0, "l", value, o, 0);
    EXPECT_TRUE(r.ok()) << r.status().ToString();
    return r.ok() ? *r : std::nullopt;
  }

  InMemoryStorage storage;
  OidAllocator oids{storage};
};

TEST_F(ListEditTest, InsertBeforeAndAfterOnBothSides) {
  Fill({"a", "b", "c", "d"});
  EXPECT_EQ(Insert(InsertWhere::kBefore, "b", "x"), 5);  // left half: shifts a left
  EXPECT_EQ(Insert(InsertWhere::kAfter, "c", "y"), 6);   // right half: shifts d right
  EXPECT_EQ(All(), (Strings{"a", "x", "b", "c", "y", "d"}));
  EXPECT_EQ(Insert(InsertWhere::kBefore, "a", "first"), 7);
  EXPECT_EQ(Insert(InsertWhere::kAfter, "d", "last"), 8);
  EXPECT_EQ(All(), (Strings{"first", "a", "x", "b", "c", "y", "d", "last"}));
  EXPECT_EQ(Insert(InsertWhere::kAfter, "nope", "z"), -1);
}

TEST_F(ListEditTest, InsertOnMissingKeyIsZero) { EXPECT_EQ(Insert(InsertWhere::kAfter, "a", "z"), 0); }

TEST_F(ListEditTest, InsertShiftsAcrossBatchBoundaries) {
  std::vector<std::string> values;
  for (int i = 0; i < 2500; ++i) values.push_back(std::to_string(i));
  Fill(std::vector<std::string_view>(values.begin(), values.end()));
  EXPECT_EQ(Insert(InsertWhere::kAfter, "1200", "L"), 2501);   // shifts 1201 elements left
  EXPECT_EQ(Insert(InsertWhere::kBefore, "1300", "R"), 2502);  // shifts 1200 elements right
  const Strings all = All();
  ASSERT_EQ(all.size(), 2502u);
  EXPECT_EQ(all[1201], "L");
  EXPECT_EQ(all[1301], "R");
  EXPECT_EQ(all.front(), "0");
  EXPECT_EQ(all.back(), "2499");
}

TEST_F(ListEditTest, RemoveFromHeadTailAndAll) {
  Fill({"a", "x", "b", "x", "c", "x"});
  EXPECT_EQ(Remove(1, "x"), 1);
  EXPECT_EQ(All(), (Strings{"a", "b", "x", "c", "x"}));
  EXPECT_EQ(Remove(-1, "x"), 1);
  EXPECT_EQ(All(), (Strings{"a", "b", "x", "c"}));
  EXPECT_EQ(Remove(0, "x"), 1);
  EXPECT_EQ(All(), (Strings{"a", "b", "c"}));
  EXPECT_EQ(Remove(0, "zz"), 0);
  EXPECT_EQ(Remove(0, "a") + Remove(0, "b") + Remove(0, "c"), 3);
  EXPECT_TRUE((*storage.Current())[CfIndex(ColumnFamily::kMeta)].empty());
  EXPECT_TRUE((*storage.Current())[CfIndex(ColumnFamily::kSubkey)].empty());
}

TEST_F(ListEditTest, TrimKeepsTheRangeAndDeletesWhenEmpty) {
  Fill({"a", "b", "c", "d", "e"});
  ASSERT_TRUE(Write([](WriteTxn& t) { return ListTrim(t, 0, "l", 1, -2, 0); }).ok());
  EXPECT_EQ(All(), (Strings{"b", "c", "d"}));
  ASSERT_TRUE(Write([](WriteTxn& t) { return ListTrim(t, 0, "l", 5, 10, 0); }).ok());
  EXPECT_TRUE((*storage.Current())[CfIndex(ColumnFamily::kMeta)].empty());
  EXPECT_TRUE((*storage.Current())[CfIndex(ColumnFamily::kSubkey)].empty());
  EXPECT_TRUE(Write([](WriteTxn& t) { return ListTrim(t, 0, "nokey", 0, 1, 0); }).ok());
}

TEST_F(ListEditTest, PosRankCountMaxlen) {
  Fill({"a", "b", "c", "b", "b", "d"});
  EXPECT_EQ(Pos("b", {}), (Positions{1}));
  EXPECT_EQ(Pos("b", {.rank = 2}), (Positions{3}));
  EXPECT_EQ(Pos("b", {.rank = -1}), (Positions{4}));
  EXPECT_EQ(Pos("b", {.rank = 1, .count = 0}), (Positions{1, 3, 4}));
  EXPECT_EQ(Pos("b", {.rank = -1, .count = 2}), (Positions{4, 3}));  // tail-first order
  EXPECT_EQ(Pos("b", {.rank = 2, .count = 0}), (Positions{3, 4}));
  EXPECT_EQ(Pos("b", {.rank = 1, .count = 0, .maxlen = 2}), (Positions{1}));
  EXPECT_EQ(Pos("b", {.rank = 4}), Positions{});
  EXPECT_EQ(Pos("z", {}), Positions{});
  auto view = storage.BeginRead();
  EXPECT_EQ(*ListPos(**view, 0, "nokey", "a", {}, 0), std::nullopt);
}

}  // namespace
}  // namespace tideskv
