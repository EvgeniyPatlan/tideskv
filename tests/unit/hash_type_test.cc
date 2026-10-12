#include "types/hash_type.h"

#include <gtest/gtest.h>

#include "storage/memory_storage.h"
#include "types/errors.h"
#include "types/object_store.h"
#include "types/oid_allocator.h"

namespace tideskv {
namespace {

struct HashTypeTest : ::testing::Test {
  void SetUp() override { ASSERT_TRUE(oids.Init().ok()); }

  template <typename Fn>
  Status Write(Fn fn) {
    auto txn = storage.BeginWrite();
    if (!txn.ok()) return txn.status();
    if (Status st = fn(**txn); !st.ok()) return st;
    return (*txn)->Commit();
  }

  int64_t Set(const std::string& key, std::vector<FieldValue> pairs) {
    int64_t added = -1;
    EXPECT_TRUE(
        Write([&](WriteTxn& t) { return MoveInto(HashSet(t, oids, 0, key, pairs, 0), &added); }).ok());
    return added;
  }

  std::optional<std::string> Get(const std::string& key, const std::string& field) {
    auto view = storage.BeginRead();
    auto v = HashGet(**view, 0, key, field, 0);
    EXPECT_TRUE(v.ok());
    return v.ok() ? *v : std::nullopt;
  }

  InMemoryStorage storage;
  OidAllocator oids{storage};
};

TEST_F(HashTypeTest, SetCountsOnlyNewFieldsAndLaterDuplicatesWin) {
  EXPECT_EQ(Set("h", {{"a", "1"}, {"b", "2"}, {"a", "3"}}), 2);
  EXPECT_EQ(Get("h", "a"), "3");
  EXPECT_EQ(Set("h", {{"b", "x"}, {"c", "y"}}), 1);
  auto view = storage.BeginRead();
  EXPECT_EQ(*HashLen(**view, 0, "h", 0), 3);
}

TEST_F(HashTypeTest, DeleteCountsDistinctExistingFieldsAndRemovesTheKeyAtZero) {
  Set("h", {{"a", "1"}, {"b", "2"}});
  int64_t n = -1;
  ASSERT_TRUE(
      Write([&](WriteTxn& t) { return MoveInto(HashDelete(t, 0, "h", {"a", "a", "zz"}, 0), &n); }).ok());
  EXPECT_EQ(n, 1);
  ASSERT_TRUE(Write([&](WriteTxn& t) { return MoveInto(HashDelete(t, 0, "h", {"b"}, 0), &n); }).ok());
  EXPECT_EQ(n, 1);
  EXPECT_TRUE((*storage.Current())[CfIndex(ColumnFamily::kMeta)].empty());
}

TEST_F(HashTypeTest, SetNxOnlyCreates) {
  bool created = false;
  ASSERT_TRUE(
      Write([&](WriteTxn& t) { return MoveInto(HashSetNx(t, oids, 0, "h", "f", "1", 0), &created); }).ok());
  EXPECT_TRUE(created);
  ASSERT_TRUE(
      Write([&](WriteTxn& t) { return MoveInto(HashSetNx(t, oids, 0, "h", "f", "2", 0), &created); }).ok());
  EXPECT_FALSE(created);
  EXPECT_EQ(Get("h", "f"), "1");
}

TEST_F(HashTypeTest, ReadsOnAMissingKey) {
  auto view = storage.BeginRead();
  EXPECT_FALSE(HashGet(**view, 0, "nope", "f", 0)->has_value());
  EXPECT_EQ(*HashLen(**view, 0, "nope", 0), 0);
  EXPECT_EQ(*HashStrlen(**view, 0, "nope", "f", 0), 0);
  EXPECT_FALSE(*HashExists(**view, 0, "nope", "f", 0));
  auto many = HashMultiGet(**view, 0, "nope", {"a", "b"}, 0);
  ASSERT_TRUE(many.ok());
  EXPECT_EQ(many->size(), 2u);
  EXPECT_FALSE((*many)[0].has_value());
}

TEST_F(HashTypeTest, WrongTypeWritesNothing) {
  ASSERT_TRUE(Write([](WriteTxn& t) { return WriteString(t, 0, "s", "v", 0, StoredObject{}); }).ok());
  auto r = [&] {
    int64_t added = 0;
    return Write([&](WriteTxn& t) { return MoveInto(HashSet(t, oids, 0, "s", {{"f", "v"}}, 0), &added); });
  }();
  EXPECT_EQ(r.message(), kErrWrongType);
  EXPECT_TRUE((*storage.Current())[CfIndex(ColumnFamily::kSubkey)].empty());
}

}  // namespace
}  // namespace tideskv
