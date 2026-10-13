#include "types/list_move.h"

#include <gtest/gtest.h>

#include "storage/memory_storage.h"
#include "types/errors.h"
#include "types/list_type.h"
#include "types/object_store.h"
#include "types/oid_allocator.h"

namespace tideskv {
namespace {

using Strings = std::vector<std::string>;

struct ListMoveTest : ::testing::Test {
  void SetUp() override { ASSERT_TRUE(oids.Init().ok()); }
  template <typename Fn>
  Status Write(Fn fn) {
    auto txn = storage.BeginWrite();
    if (!txn.ok()) return txn.status();
    if (Status st = fn(**txn); !st.ok()) return st;
    return (*txn)->Commit();
  }
  void Fill(std::string_view key, std::vector<std::string_view> values) {
    ASSERT_TRUE(Write([&](WriteTxn& t) {
                  return ListPush(t, oids, 0, key, ListEnd::kRight, values, false, 0).status();
                }).ok());
  }
  Strings All(std::string_view key) {
    auto view = storage.BeginRead();
    return *ListRange(**view, 0, key, 0, -1, 0);
  }
  std::optional<std::string> Move(std::string_view src, std::string_view dst, ListEnd from, ListEnd to) {
    std::optional<std::string> out;
    EXPECT_TRUE(
        Write([&](WriteTxn& t) { return MoveInto(ListMove(t, oids, 0, src, dst, from, to, 0), &out); }).ok());
    return out;
  }

  InMemoryStorage storage;
  OidAllocator oids{storage};
};

TEST_F(ListMoveTest, MovesBetweenKeysAndCreatesDst) {
  Fill("a", {"1", "2", "3"});
  EXPECT_EQ(Move("a", "b", ListEnd::kRight, ListEnd::kLeft), "3");
  EXPECT_EQ(Move("a", "b", ListEnd::kLeft, ListEnd::kRight), "1");
  EXPECT_EQ(All("a"), (Strings{"2"}));
  EXPECT_EQ(All("b"), (Strings{"3", "1"}));
  EXPECT_EQ(Move("a", "b", ListEnd::kLeft, ListEnd::kLeft), "2");
  EXPECT_EQ(All("a"), Strings{});  // src deleted when emptied
  EXPECT_EQ(Move("a", "b", ListEnd::kLeft, ListEnd::kLeft), std::nullopt);
}

TEST_F(ListMoveTest, SameKeyRotates) {
  Fill("a", {"1", "2", "3"});
  EXPECT_EQ(Move("a", "a", ListEnd::kRight, ListEnd::kLeft), "3");
  EXPECT_EQ(All("a"), (Strings{"3", "1", "2"}));
  EXPECT_EQ(Move("a", "a", ListEnd::kLeft, ListEnd::kLeft), "3");  // no-op, returns the head
  EXPECT_EQ(All("a"), (Strings{"3", "1", "2"}));
  Fill("one", {"x"});
  EXPECT_EQ(Move("one", "one", ListEnd::kLeft, ListEnd::kRight), "x");
  EXPECT_EQ(All("one"), (Strings{"x"}));  // a one-element rotation keeps the key
}

TEST_F(ListMoveTest, WrongTypeDstLeavesSrcIntact) {
  Fill("a", {"1"});
  ASSERT_TRUE(Write([](WriteTxn& t) { return WriteString(t, 0, "s", "v", 0, StoredObject{}); }).ok());
  EXPECT_EQ(Write([&](WriteTxn& t) {
              return ListMove(t, oids, 0, "a", "s", ListEnd::kLeft, ListEnd::kLeft, 0).status();
            }).message(),
            kErrWrongType);
  EXPECT_EQ(All("a"), (Strings{"1"}));
}

TEST_F(ListMoveTest, PopFirstScansKeysInOrder) {
  Fill("b", {"1", "2", "3"});
  std::optional<PoppedFrom> got;
  ASSERT_TRUE(Write([&](WriteTxn& t) {
                return MoveInto(ListPopFirst(t, 0, {"missing", "b"}, ListEnd::kRight, 2, 0), &got);
              }).ok());
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->key, "b");
  EXPECT_EQ(got->values, (Strings{"3", "2"}));
  ASSERT_TRUE(Write([&](WriteTxn& t) {
                return MoveInto(ListPopFirst(t, 0, {"x", "y"}, ListEnd::kLeft, 1, 0), &got);
              }).ok());
  EXPECT_FALSE(got.has_value());
  ASSERT_TRUE(Write([](WriteTxn& t) { return WriteString(t, 0, "s", "v", 0, StoredObject{}); }).ok());
  EXPECT_EQ(Write([&](WriteTxn& t) {
              return ListPopFirst(t, 0, {"x", "s", "b"}, ListEnd::kLeft, 1, 0).status();
            }).message(),
            kErrWrongType);  // a wrong-typed key before the first list aborts
}

TEST_F(ListMoveTest, PopFirstPopsALargeCountInOrder) {
  std::vector<std::string> values;
  for (int i = 0; i < 2000; ++i) values.push_back("v" + std::to_string(i));
  Fill("b", std::vector<std::string_view>(values.begin(), values.end()));
  std::optional<PoppedFrom> got;
  ASSERT_TRUE(Write([&](WriteTxn& t) {
                return MoveInto(ListPopFirst(t, 0, {"b"}, ListEnd::kRight, 1500, 0), &got);
              }).ok());
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->values, Strings(values.rbegin(), values.rbegin() + 1500));
  EXPECT_EQ(All("b"), Strings(values.begin(), values.begin() + 500));
  ASSERT_TRUE(Write([&](WriteTxn& t) {
                return MoveInto(ListPopFirst(t, 0, {"b"}, ListEnd::kLeft, 9999, 0), &got);
              }).ok());
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->values, Strings(values.begin(), values.begin() + 500));
  EXPECT_TRUE((*storage.Current())[CfIndex(ColumnFamily::kMeta)].empty());  // the key went with it
  EXPECT_TRUE((*storage.Current())[CfIndex(ColumnFamily::kSubkey)].empty());
}

}  // namespace
}  // namespace tideskv
