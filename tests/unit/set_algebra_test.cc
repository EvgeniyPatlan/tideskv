#include "types/set_algebra.h"

#include <gtest/gtest.h>

#include "storage/memory_storage.h"
#include "types/errors.h"
#include "types/oid_allocator.h"
#include "types/set_type.h"

namespace tideskv {
namespace {

struct SetAlgebraTest : ::testing::Test {
  void SetUp() override {
    ASSERT_TRUE(oids.Init().ok());
    Add("a", {"1", "2", "3", "4"});
    Add("b", {"2", "3", "5"});
    Add("c", {"3", "4", "6"});
  }
  void Add(const std::string& key, const std::vector<std::string_view>& m) {
    auto txn = storage.BeginWrite();
    int64_t n = 0;
    ASSERT_TRUE(MoveInto(SetAdd(**txn, oids, 0, key, m, 0), &n).ok());
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  std::vector<std::string> Combine(SetOp op, const std::vector<std::string_view>& keys) {
    auto view = storage.BeginRead();
    auto r = SetCombine(**view, 0, op, keys, 0);
    EXPECT_TRUE(r.ok()) << r.status().ToString();
    return r.ok() ? *r : std::vector<std::string>{};
  }
  InMemoryStorage storage;
  OidAllocator oids{storage};
};

TEST_F(SetAlgebraTest, InterUnionDiff) {
  EXPECT_EQ(Combine(SetOp::kInter, {"a", "b"}), (std::vector<std::string>{"2", "3"}));
  EXPECT_EQ(Combine(SetOp::kInter, {"a", "b", "c"}), (std::vector<std::string>{"3"}));
  EXPECT_EQ(Combine(SetOp::kUnion, {"b", "c"}), (std::vector<std::string>{"2", "3", "4", "5", "6"}));
  EXPECT_EQ(Combine(SetOp::kDiff, {"a", "b", "c"}), (std::vector<std::string>{"1"}));
  EXPECT_EQ(Combine(SetOp::kDiff, {"a", "a"}), (std::vector<std::string>{}));
}

TEST_F(SetAlgebraTest, MissingKeysAreEmptySets) {
  EXPECT_TRUE(Combine(SetOp::kInter, {"a", "nope"}).empty());
  EXPECT_EQ(Combine(SetOp::kUnion, {"nope", "b"}), (std::vector<std::string>{"2", "3", "5"}));
  EXPECT_TRUE(Combine(SetOp::kDiff, {"nope", "a"}).empty());
  EXPECT_EQ(Combine(SetOp::kDiff, {"b", "nope"}), (std::vector<std::string>{"2", "3", "5"}));
}

TEST_F(SetAlgebraTest, InterCardHonoursTheLimit) {
  auto view = storage.BeginRead();
  EXPECT_EQ(*SetInterCard(**view, 0, {"a", "b"}, 0, 0), 2);
  EXPECT_EQ(*SetInterCard(**view, 0, {"a", "b"}, 1, 0), 1);
  EXPECT_EQ(*SetInterCard(**view, 0, {"a", "nope"}, 0, 0), 0);
}

}  // namespace
}  // namespace tideskv
