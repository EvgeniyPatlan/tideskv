#include "types/collection.h"

#include <gtest/gtest.h>

#include "encoding/key_codec.h"
#include "storage/memory_storage.h"
#include "types/errors.h"
#include "types/object_store.h"
#include "types/oid_allocator.h"

namespace tideskv {
namespace {

using encoding::ValueType;

struct CollectionTest : ::testing::Test {
  void SetUp() override { ASSERT_TRUE(oids.Init().ok()); }

  // Creates a hash with the given fields in one committed transaction.
  void MakeHash(const std::string& key, std::initializer_list<std::pair<std::string, std::string>> fields,
                int64_t expire_ms = 0) {
    auto txn = storage.BeginWrite();
    ASSERT_TRUE(txn.ok());
    auto c = LoadCollectionForWrite(**txn, 0, key, ValueType::kHash, 0);
    ASSERT_TRUE(c.ok());
    ASSERT_TRUE(InitCollection(&*c, ValueType::kHash, oids).ok());
    for (const auto& [f, v] : fields) {
      ASSERT_TRUE((*txn)->Put(ColumnFamily::kSubkey, c->SubkeyFor(f), v).ok());
      ++c->obj.meta.size;
    }
    c->obj.meta.expire_ms = expire_ms;
    ASSERT_TRUE(SaveCollection(**txn, *c).ok());
    if (expire_ms != 0) {
      ASSERT_TRUE((*txn)
                      ->Put(ColumnFamily::kExpire,
                            encoding::ExpireKeyFor(0, static_cast<uint64_t>(expire_ms), key),
                            ExpireValue(c->obj.meta.oid))
                      .ok());
    }
    ASSERT_TRUE((*txn)->Commit().ok());
  }

  size_t Rows(ColumnFamily cf) { return (*storage.Current())[CfIndex(cf)].size(); }

  InMemoryStorage storage;
  OidAllocator oids{storage};
};

TEST_F(CollectionTest, MissingKeyIsAnEmptyUnstoredCollection) {
  auto view = storage.BeginRead();
  auto c = LoadCollection(**view, 0, "nope", ValueType::kHash, 0);
  ASSERT_TRUE(c.ok());
  EXPECT_FALSE(c->exists());
  EXPECT_FALSE(c->stored);
  EXPECT_EQ(c->size(), 0u);
}

TEST_F(CollectionTest, WrongTypeIsAUserError) {
  MakeHash("h", {{"f", "v"}});
  auto view = storage.BeginRead();
  auto c = LoadCollection(**view, 0, "h", ValueType::kSet, 0);
  ASSERT_FALSE(c.ok());
  EXPECT_EQ(c.status().code(), StatusCode::kUserError);
  EXPECT_EQ(c.status().message(), kErrWrongType);
}

TEST_F(CollectionTest, StringKeyIsWrongType) {
  auto txn = storage.BeginWrite();
  ASSERT_TRUE(WriteString(**txn, 0, "s", "v", 0, StoredObject{}).ok());
  ASSERT_TRUE((*txn)->Commit().ok());
  auto view = storage.BeginRead();
  EXPECT_EQ(LoadCollection(**view, 0, "s", ValueType::kHash, 0).status().message(), kErrWrongType);
}

TEST_F(CollectionTest, ElementsIterateInByteOrderWithinTheirOwnPrefix) {
  MakeHash("a", {{"z", "1"}, {"m", "2"}, {"", "3"}});
  MakeHash("b", {{"x", "9"}});  // a different oid right after a's: must not leak in
  auto view = storage.BeginRead();
  auto c = LoadCollection(**view, 0, "a", ValueType::kHash, 0);
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(c->size(), 3u);
  std::vector<std::string> seen;
  ASSERT_TRUE(ForEachElement(**view, *c, [&](std::string_view f, std::string_view v) {
                seen.push_back(std::string(f) + "=" + std::string(v));
                return true;
              }).ok());
  EXPECT_EQ(seen, (std::vector<std::string>{"=3", "m=2", "z=1"}));
}

TEST_F(CollectionTest, ForEachStopsWhenAsked) {
  MakeHash("a", {{"1", ""}, {"2", ""}, {"3", ""}});
  auto view = storage.BeginRead();
  auto c = LoadCollection(**view, 0, "a", ValueType::kHash, 0);
  int n = 0;
  ASSERT_TRUE(ForEachElement(**view, *c, [&](std::string_view, std::string_view) { return ++n < 2; }).ok());
  EXPECT_EQ(n, 2);
}

TEST_F(CollectionTest, GetAndHasElement) {
  MakeHash("h", {{"f", "v"}});
  auto view = storage.BeginRead();
  auto c = LoadCollection(**view, 0, "h", ValueType::kHash, 0);
  auto present = GetElement(**view, *c, "f");
  ASSERT_TRUE(present.ok());
  const auto& value = present.value();
  if (!value.has_value()) FAIL() << "field f missing";
  EXPECT_EQ(*value, "v");
  auto missing = GetElement(**view, *c, "g");
  ASSERT_TRUE(missing.ok());
  EXPECT_FALSE(missing->has_value());
  EXPECT_TRUE(*HasElement(**view, *c, "f"));
  EXPECT_FALSE(*HasElement(**view, *c, "g"));
}

TEST_F(CollectionTest, PutAndRemoveElementKeepTheSizeExact) {
  auto txn = storage.BeginWrite();
  auto c = OpenCollectionForWrite(**txn, oids, 0, "h", ValueType::kHash, 0);
  ASSERT_TRUE(c.ok());
  EXPECT_TRUE(*PutElement(**txn, *c, "f", "1"));
  EXPECT_FALSE(*PutElement(**txn, *c, "f", "2"));  // overwrite: not new
  EXPECT_TRUE(*PutElement(**txn, *c, "g", "3"));
  EXPECT_EQ(c->size(), 2u);
  EXPECT_TRUE(*RemoveElement(**txn, *c, "f"));
  EXPECT_FALSE(*RemoveElement(**txn, *c, "f"));  // already gone in this txn
  EXPECT_EQ(c->size(), 1u);
  ASSERT_TRUE(SaveCollection(**txn, *c).ok());
  ASSERT_TRUE((*txn)->Commit().ok());
  EXPECT_EQ(Rows(ColumnFamily::kSubkey), 1u);
}

TEST_F(CollectionTest, SavingSizeZeroDeletesTheKeyWithoutAPrefixTombstone) {
  MakeHash("h", {{"f", "v"}}, /*expire_ms=*/5000);
  auto txn = storage.BeginWrite();
  auto c = LoadCollectionForWrite(**txn, 0, "h", ValueType::kHash, 0);
  ASSERT_TRUE(c.ok());
  ASSERT_TRUE((*txn)->Delete(ColumnFamily::kSubkey, c->SubkeyFor("f")).ok());
  c->obj.meta.size = 0;
  ASSERT_TRUE(SaveCollection(**txn, *c).ok());
  ASSERT_TRUE((*txn)->Commit().ok());
  EXPECT_EQ(Rows(ColumnFamily::kMeta), 0u);
  EXPECT_EQ(Rows(ColumnFamily::kExpire), 0u);  // Review Focus #2: no expire residue
  EXPECT_EQ(Rows(ColumnFamily::kSubkey), 0u);
}

TEST_F(CollectionTest, SavingAnUnstoredEmptyCollectionWritesNothing) {
  auto txn = storage.BeginWrite();
  auto c = LoadCollectionForWrite(**txn, 0, "h", ValueType::kSet, 0);
  ASSERT_TRUE(InitCollection(&*c, ValueType::kSet, oids).ok());
  ASSERT_TRUE(SaveCollection(**txn, *c).ok());
  ASSERT_TRUE((*txn)->Commit().ok());
  EXPECT_EQ(Rows(ColumnFamily::kMeta), 0u);
}

TEST_F(CollectionTest, ExpiredCollectionLoadsAsMissingAndWritersGetAFreshOid) {  // Review Focus #1
  MakeHash("h", {{"old", "v"}}, /*expire_ms=*/100);
  auto view = storage.BeginRead();
  auto expired = LoadCollection(**view, 0, "h", ValueType::kHash, /*now_ms=*/100);
  ASSERT_TRUE(expired.ok());
  EXPECT_FALSE(expired->exists());
  auto txn = storage.BeginWrite();
  auto c = LoadCollectionForWrite(**txn, 0, "h", ValueType::kHash, 100);
  ASSERT_TRUE(c.ok());
  EXPECT_FALSE(c->exists());
  ASSERT_TRUE(InitCollection(&*c, ValueType::kHash, oids).ok());
  EXPECT_FALSE(GetElement(**txn, *c, "old")->has_value());  // new oid, new prefix
}

TEST_F(CollectionTest, LoadForWriteOnAnExpiredCollectionDropsItsData) {  // M2 Task 6 carry-over
  MakeHash("h", {{"a", "1"}, {"b", "2"}}, /*expire_ms=*/100);
  auto txn = storage.BeginWrite();
  ASSERT_TRUE(LoadCollectionForWrite(**txn, 0, "h", ValueType::kHash, 100).ok());
  ASSERT_TRUE((*txn)->Commit().ok());
  EXPECT_EQ(Rows(ColumnFamily::kMeta), 0u);
  EXPECT_EQ(Rows(ColumnFamily::kExpire), 0u);
  EXPECT_EQ(Rows(ColumnFamily::kSubkey), 0u);
}

TEST_F(CollectionTest, IterateRangeVisitsTheSampledPositions) {
  MakeHash("h", {{"a", "1"}, {"b", "2"}, {"c", "3"}});
  auto view = storage.BeginRead();
  auto c = LoadCollection(**view, 0, "h", ValueType::kHash, 0);
  std::vector<std::string> got;
  ASSERT_TRUE(IterateRange(**view, *c, {0, 2, 2}, [&](std::string_view f, std::string_view) {
                got.emplace_back(f);
              }).ok());
  EXPECT_EQ(got, (std::vector<std::string>{"a", "c", "c"}));
}

TEST_F(CollectionTest, IterateRangePastTheStoredElementsIsCorruption) {
  MakeHash("h", {{"a", "1"}, {"b", "2"}});
  auto view = storage.BeginRead();
  auto c = LoadCollection(**view, 0, "h", ValueType::kHash, 0);
  ASSERT_TRUE(c.ok());
  c->obj.meta.size = 3;  // meta claims an element the subkey CF does not hold
  std::vector<std::string> got;
  const Status st =
      IterateRange(**view, *c, {1, 2}, [&](std::string_view f, std::string_view) { got.emplace_back(f); });
  EXPECT_EQ(st.code(), StatusCode::kCorruption);
  EXPECT_EQ(got, (std::vector<std::string>{"b"}));  // the positions that do exist were visited
}

TEST_F(CollectionTest, RandomElementsDistinctAndWithRepeats) {
  MakeHash("h", {{"a", "1"}, {"b", "2"}, {"c", "3"}});
  auto view = storage.BeginRead();
  auto c = LoadCollection(**view, 0, "h", ValueType::kHash, 0);
  std::mt19937_64 rng(5);
  auto all = RandomElements(**view, *c, 10, rng);
  ASSERT_TRUE(all.ok());
  ASSERT_EQ(all->size(), 3u);  // count >= size: the whole hash, in byte order
  EXPECT_EQ((*all)[0].field, "a");
  EXPECT_EQ((*all)[2].value, "3");
  auto two = RandomElements(**view, *c, 2, rng);
  ASSERT_EQ(two->size(), 2u);
  EXPECT_NE((*two)[0].field, (*two)[1].field);
  auto rep = RandomElements(**view, *c, -20, rng);
  ASSERT_EQ(rep->size(), 20u);
  for (const Element& e : *rep) EXPECT_TRUE(e.field == "a" || e.field == "b" || e.field == "c");
  auto none = RandomElements(**view, *c, 0, rng);
  EXPECT_TRUE(none->empty());
}

}  // namespace
}  // namespace tideskv
