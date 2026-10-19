#include "types/list_type.h"

#include <gtest/gtest.h>

#include "encoding/key_codec.h"
#include "encoding/meta_codec.h"
#include "storage/memory_storage.h"
#include "types/errors.h"
#include "types/oid_allocator.h"

namespace tideskv {
namespace {

using Strings = std::vector<std::string>;

struct ListTypeTest : ::testing::Test {
  void SetUp() override { ASSERT_TRUE(oids.Init().ok()); }

  template <typename Fn>
  Status Write(Fn fn) {
    auto txn = storage.BeginWrite();
    if (!txn.ok()) return txn.status();
    if (Status st = fn(**txn); !st.ok()) return st;
    return (*txn)->Commit();
  }

  int64_t Push(ListEnd end, std::vector<std::string_view> values, bool xx = false) {
    int64_t n = -1;
    EXPECT_TRUE(
        Write([&](WriteTxn& t) { return MoveInto(ListPush(t, oids, 0, "l", end, values, xx, 0), &n); }).ok());
    return n;
  }
  std::optional<Strings> Pop(ListEnd end, uint64_t count) {
    std::optional<Strings> out;
    EXPECT_TRUE(Write([&](WriteTxn& t) { return MoveInto(ListPop(t, 0, "l", end, count, 0), &out); }).ok());
    return out;
  }
  Strings Range(int64_t start, int64_t stop) {
    auto view = storage.BeginRead();
    auto r = ListRange(**view, 0, "l", start, stop, 0);
    EXPECT_TRUE(r.ok()) << r.status().ToString();
    return r.ok() ? *r : Strings{};
  }

  encoding::Metadata Meta() {
    auto view = storage.BeginRead();
    auto c = LoadCollection(**view, 0, "l", encoding::ValueType::kList, 0);
    EXPECT_TRUE(c.ok() && c->exists());
    return c.ok() ? c->obj.meta : encoding::Metadata{};
  }

  InMemoryStorage storage;
  OidAllocator oids{storage};
};

TEST_F(ListTypeTest, PushesAtBothEndsKeepOrder) {
  EXPECT_EQ(Push(ListEnd::kRight, {"b", "c"}), 2);
  EXPECT_EQ(Push(ListEnd::kLeft, {"a", "z"}), 4);  // LPUSH z after a: z ends up first
  EXPECT_EQ(Range(0, -1), (Strings{"z", "a", "b", "c"}));
  auto view = storage.BeginRead();
  EXPECT_EQ(*ListLen(**view, 0, "l", 0), 4);
}

TEST_F(ListTypeTest, PushxOnMissingKeyWritesNothing) {
  EXPECT_EQ(Push(ListEnd::kLeft, {"a"}, /*xx=*/true), 0);
  EXPECT_TRUE((*storage.Current())[CfIndex(ColumnFamily::kMeta)].empty());
  EXPECT_EQ(Push(ListEnd::kLeft, {"a"}), 1);
  EXPECT_EQ(Push(ListEnd::kRight, {"b", "c"}, /*xx=*/true), 3);
}

TEST_F(ListTypeTest, PopFromBothEndsAndDeleteWhenEmpty) {
  Push(ListEnd::kRight, {"a", "b", "c", "d"});
  EXPECT_EQ(Pop(ListEnd::kLeft, 1), (Strings{"a"}));
  EXPECT_EQ(Pop(ListEnd::kRight, 2), (Strings{"d", "c"}));  // pop order
  EXPECT_EQ(Pop(ListEnd::kRight, 0), Strings{});            // existing key, count 0
  EXPECT_EQ(Pop(ListEnd::kLeft, 10), (Strings{"b"}));
  EXPECT_EQ(Pop(ListEnd::kLeft, 1), std::nullopt);  // the key is gone
  const auto tables = storage.Current();
  EXPECT_TRUE((*tables)[CfIndex(ColumnFamily::kMeta)].empty());
  EXPECT_TRUE((*tables)[CfIndex(ColumnFamily::kSubkey)].empty());
}

TEST_F(ListTypeTest, PopsALargeCountFromEitherEndInOneRun) {
  std::vector<std::string> values;
  for (int i = 0; i < 6000; ++i) values.push_back("v" + std::to_string(i));
  const std::vector<std::string_view> views(values.begin(), values.end());
  ASSERT_EQ(Push(ListEnd::kRight, views), 6000);
  const encoding::Metadata before = Meta();

  const auto left = Pop(ListEnd::kLeft, 5000);
  ASSERT_TRUE(left.has_value());
  EXPECT_EQ(*left, Strings(values.begin(), values.begin() + 5000));
  EXPECT_EQ(Range(0, -1), Strings(values.begin() + 5000, values.end()));
  encoding::Metadata m = Meta();
  EXPECT_EQ(m.head, before.head + 5000);
  EXPECT_EQ(m.tail, before.tail);
  EXPECT_EQ(m.size, 1000u);
  EXPECT_EQ((*storage.Current())[CfIndex(ColumnFamily::kSubkey)].size(), 1000u);

  const auto right = Pop(ListEnd::kRight, 600);  // tail first
  ASSERT_TRUE(right.has_value());
  EXPECT_EQ(*right, Strings(values.rbegin(), values.rbegin() + 600));
  EXPECT_EQ(Range(0, -1), Strings(values.begin() + 5000, values.end() - 600));
  m = Meta();
  EXPECT_EQ(m.head, before.head + 5000);
  EXPECT_EQ(m.tail, before.tail - 600);
  EXPECT_EQ(m.size, 400u);
  EXPECT_EQ((*storage.Current())[CfIndex(ColumnFamily::kSubkey)].size(), 400u);
}

// Counts the iterators a read opens: on TidesDB an iterator references every table and checks the range
// tombstones, so a short run is read with point lookups instead.
class CountingView final : public ReadView {
 public:
  explicit CountingView(ReadView& inner) : inner_(inner) {}
  Result<std::string> Get(ColumnFamily cf, std::string_view key) override { return inner_.Get(cf, key); }
  Result<std::unique_ptr<Iterator>> NewIterator(ColumnFamily cf, IterOptions opts) override {
    ++iterators;
    return inner_.NewIterator(cf, std::move(opts));
  }
  int iterators = 0;

 private:
  ReadView& inner_;
};

TEST_F(ListTypeTest, ShortRangesAreReadWithPointLookups) {
  std::vector<std::string> values;
  for (int i = 0; i < 100; ++i) values.push_back("v" + std::to_string(i));
  ASSERT_EQ(Push(ListEnd::kRight, std::vector<std::string_view>(values.begin(), values.end())), 100);
  auto view = storage.BeginRead();
  auto c = LoadCollection(**view, 0, "l", encoding::ValueType::kList, 0);
  ASSERT_TRUE(c.ok() && c->exists());
  CountingView counting(**view);
  Strings out;
  ASSERT_TRUE(ReadListRange(counting, *c, 3, 3, &out).ok());
  ASSERT_TRUE(ReadListRange(counting, *c, 10, 17, &out).ok());
  EXPECT_EQ(counting.iterators, 0);
  EXPECT_EQ(out, (Strings{"v3", "v10", "v11", "v12", "v13", "v14", "v15", "v16", "v17"}));
  out.clear();
  ASSERT_TRUE(ReadListRange(counting, *c, 0, 99, &out).ok());  // a long run still streams
  EXPECT_EQ(counting.iterators, 1);
  EXPECT_EQ(out, values);
}

TEST_F(ListTypeTest, AShortRangeMissingAnElementIsCorruption) {
  Push(ListEnd::kRight, {"a", "b", "c"});
  auto view = storage.BeginRead();
  auto c = LoadCollection(**view, 0, "l", encoding::ValueType::kList, 0);
  ASSERT_TRUE(c.ok() && c->exists());
  ASSERT_TRUE(
      Write([&](WriteTxn& t) { return t.Delete(ColumnFamily::kSubkey, ListElementKey(*c, 1)); }).ok());
  auto fresh = storage.BeginRead();
  Strings out;
  EXPECT_EQ(ReadListRange(**fresh, *c, 0, 2, &out).code(), StatusCode::kCorruption);
}

TEST_F(ListTypeTest, RangeFollowsRedisIndexRules) {
  Push(ListEnd::kRight, {"a", "b", "c", "d", "e"});
  EXPECT_EQ(Range(1, 2), (Strings{"b", "c"}));
  EXPECT_EQ(Range(-2, -1), (Strings{"d", "e"}));
  EXPECT_EQ(Range(-100, 1), (Strings{"a", "b"}));  // start clamps to 0
  EXPECT_EQ(Range(3, 100), (Strings{"d", "e"}));   // stop clamps to len-1
  EXPECT_EQ(Range(3, 2), Strings{});
  EXPECT_EQ(Range(5, 9), Strings{});
  EXPECT_EQ(Range(0, -6), Strings{});
}

TEST_F(ListTypeTest, IndexAndSet) {
  Push(ListEnd::kRight, {"a", "b", "c"});
  auto view = storage.BeginRead();
  EXPECT_EQ(*ListIndex(**view, 0, "l", "0", 0), "a");
  EXPECT_EQ(*ListIndex(**view, 0, "l", "-1", 0), "c");
  EXPECT_EQ(*ListIndex(**view, 0, "l", "3", 0), std::nullopt);
  EXPECT_EQ(*ListIndex(**view, 0, "l", "-4", 0), std::nullopt);
  EXPECT_EQ(ListIndex(**view, 0, "l", "x", 0).status().message(), kErrNotInteger);
  EXPECT_EQ(*ListIndex(**view, 0, "nokey", "x", 0), std::nullopt);  // the key is looked up first
  EXPECT_TRUE(Write([](WriteTxn& t) { return ListSet(t, 0, "l", "-2", "B", 0); }).ok());
  EXPECT_EQ(Range(0, -1), (Strings{"a", "B", "c"}));
  EXPECT_EQ(Write([](WriteTxn& t) { return ListSet(t, 0, "l", "3", "x", 0); }).message(),
            kErrIndexOutOfRange);
  EXPECT_EQ(Write([](WriteTxn& t) { return ListSet(t, 0, "nokey", "x", "v", 0); }).message(), kErrNoSuchKey);
}

TEST_F(ListTypeTest, NormalizeListRange) {
  uint64_t from = 0, to = 0;
  EXPECT_TRUE(NormalizeListRange(0, -1, 3, &from, &to));
  EXPECT_EQ(from, 0u);
  EXPECT_EQ(to, 2u);
  EXPECT_FALSE(NormalizeListRange(0, 0, 0, &from, &to));
  EXPECT_FALSE(NormalizeListRange(INT64_MIN, INT64_MIN, 3, &from, &to));
  EXPECT_TRUE(NormalizeListRange(INT64_MIN, INT64_MAX, 3, &from, &to));
  EXPECT_EQ(from, 0u);
  EXPECT_EQ(to, 2u);
}

TEST_F(ListTypeTest, WrongType) {
  ASSERT_TRUE(Write([](WriteTxn& t) {
                return t.Put(ColumnFamily::kMeta, encoding::MetaKeyFor(0, "l"),
                             encoding::EncodeMetadata(encoding::Metadata{}, "str"));
              }).ok());
  auto view = storage.BeginRead();
  EXPECT_EQ(ListLen(**view, 0, "l", 0).status().message(), kErrWrongType);
  EXPECT_EQ(Write([&](WriteTxn& t) {
              return ListPush(t, oids, 0, "l", ListEnd::kLeft, {"a"}, false, 0).status();
            }).message(),
            kErrWrongType);
}

}  // namespace
}  // namespace tideskv
