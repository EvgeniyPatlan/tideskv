#include "types/set_type.h"

#include <gtest/gtest.h>

#include "storage/memory_storage.h"
#include "types/errors.h"
#include "types/oid_allocator.h"

namespace tideskv {
namespace {

struct SetTypeTest : ::testing::Test {
  void SetUp() override { ASSERT_TRUE(oids.Init().ok()); }

  template <typename Fn>
  Status Write(Fn fn) {
    auto txn = storage.BeginWrite();
    if (!txn.ok()) return txn.status();
    if (Status st = fn(**txn); !st.ok()) return st;
    return (*txn)->Commit();
  }

  int64_t Add(const std::string& key, std::vector<std::string_view> members) {
    int64_t n = -1;
    EXPECT_TRUE(Write([&](WriteTxn& t) { return MoveInto(SetAdd(t, oids, 0, key, members, 0), &n); }).ok());
    return n;
  }

  InMemoryStorage storage;
  OidAllocator oids{storage};
};

TEST_F(SetTypeTest, AddCountsNewMembersOnce) {
  EXPECT_EQ(Add("s", {"a", "b", "a"}), 2);
  EXPECT_EQ(Add("s", {"b", "c"}), 1);
  auto view = storage.BeginRead();
  EXPECT_EQ(*SetCard(**view, 0, "s", 0), 3);
  EXPECT_EQ(*SetMembers(**view, 0, "s", 0), (std::vector<std::string>{"a", "b", "c"}));
}

TEST_F(SetTypeTest, AddingAnExistingMemberWritesNothing) {
  Add("s", {"a"});
  const auto before = storage.Current();
  Add("s", {"a"});
  EXPECT_EQ(*storage.Current(), *before);  // no meta or subkey rewrite
}

TEST_F(SetTypeTest, RemoveToZeroDeletesTheKey) {
  Add("s", {"a", "b"});
  int64_t n = -1;
  ASSERT_TRUE(
      Write([&](WriteTxn& t) { return MoveInto(SetRemove(t, 0, "s", {"a", "a", "x", "b"}, 0), &n); }).ok());
  EXPECT_EQ(n, 2);
  EXPECT_TRUE((*storage.Current())[CfIndex(ColumnFamily::kMeta)].empty());
  EXPECT_TRUE((*storage.Current())[CfIndex(ColumnFamily::kSubkey)].empty());
}

TEST_F(SetTypeTest, MultiIsMemberOnMissingKeyIsAllZeros) {
  auto view = storage.BeginRead();
  EXPECT_EQ(*SetMultiIsMember(**view, 0, "nope", {"a", "b"}, 0), (std::vector<int64_t>{0, 0}));
}

TEST_F(SetTypeTest, PopRemovesWhatItReturns) {
  Add("s", {"a", "b", "c", "d"});
  std::mt19937_64 rng(9);
  std::vector<std::string> popped;
  ASSERT_TRUE(Write([&](WriteTxn& t) { return MoveInto(SetPop(t, 0, "s", 3, rng, 0), &popped); }).ok());
  ASSERT_EQ(popped.size(), 3u);
  auto view = storage.BeginRead();
  auto rest = SetMembers(**view, 0, "s", 0);
  ASSERT_EQ(rest->size(), 1u);
  for (const auto& p : popped) EXPECT_NE(p, (*rest)[0]);
  EXPECT_EQ(*SetCard(**view, 0, "s", 0), 1);
}

TEST_F(SetTypeTest, PopEverythingDeletesTheKeyWithPointDeletes) {
  Add("s", {"a", "b"});
  std::mt19937_64 rng(1);
  std::vector<std::string> popped;
  ASSERT_TRUE(Write([&](WriteTxn& t) { return MoveInto(SetPop(t, 0, "s", 99, rng, 0), &popped); }).ok());
  EXPECT_EQ(popped, (std::vector<std::string>{"a", "b"}));
  EXPECT_TRUE((*storage.Current())[CfIndex(ColumnFamily::kMeta)].empty());
  EXPECT_TRUE((*storage.Current())[CfIndex(ColumnFamily::kSubkey)].empty());
}

TEST_F(SetTypeTest, MoveSemantics) {
  Add("src", {"a", "b"});
  int64_t moved = -1;
  ASSERT_TRUE(
      Write([&](WriteTxn& t) { return MoveInto(SetMove(t, oids, 0, "src", "dst", "a", 0), &moved); }).ok());
  EXPECT_EQ(moved, 1);
  ASSERT_TRUE(
      Write([&](WriteTxn& t) { return MoveInto(SetMove(t, oids, 0, "src", "dst", "zz", 0), &moved); }).ok());
  EXPECT_EQ(moved, 0);
  ASSERT_TRUE(
      Write([&](WriteTxn& t) { return MoveInto(SetMove(t, oids, 0, "src", "dst", "b", 0), &moved); }).ok());
  EXPECT_EQ(moved, 1);  // src is now empty and gone
  auto view = storage.BeginRead();
  EXPECT_EQ(*SetCard(**view, 0, "src", 0), 0);
  EXPECT_EQ(*SetMembers(**view, 0, "dst", 0), (std::vector<std::string>{"a", "b"}));
}

}  // namespace
}  // namespace tideskv
