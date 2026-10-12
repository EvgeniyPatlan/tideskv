#include "types/object_store.h"

#include <gtest/gtest.h>

#include "common/crc16.h"
#include "encoding/key_codec.h"
#include "encoding/object_codec.h"
#include "storage/memory_storage.h"

namespace tideskv {
namespace {

using encoding::ExpireKeyFor;
using encoding::Metadata;
using encoding::MetaKeyFor;
using encoding::ValueType;

constexpr int64_t kNow = 1'700'000'000'000;

struct ObjectStoreTest : ::testing::Test {
  InMemoryStorage storage;

  template <typename Fn>
  void Write(Fn fn) {
    auto txn = storage.BeginWrite();
    ASSERT_TRUE(txn.ok());
    ASSERT_TRUE(fn(**txn).ok());
    ASSERT_TRUE((*txn)->Commit().ok());
  }

  StoredObject Load(uint8_t db, const std::string& key, int64_t now = kNow) {
    auto view = storage.BeginRead();
    auto obj = LoadObject(**view, db, key, now);
    EXPECT_TRUE(obj.ok()) << obj.status().ToString();
    return obj.ok() ? *obj : StoredObject{};
  }

  const InMemoryStorage::Table& Cf(ColumnFamily cf) {
    tables_ = storage.Current();
    return (*tables_)[CfIndex(cf)];
  }

  void PutString(uint8_t db, const std::string& key, const std::string& value, int64_t expire_ms) {
    Write([&](WriteTxn& t) {
      auto old = LoadForWrite(t, db, key, kNow);
      if (!old.ok()) return old.status();
      return WriteString(t, db, key, value, expire_ms, *old);
    });
  }

  std::shared_ptr<const InMemoryStorage::Tables> tables_;
};

TEST_F(ObjectStoreTest, MissingKey) { EXPECT_EQ(Load(0, "nope").state, ObjectState::kMissing); }

TEST_F(ObjectStoreTest, StringRoundTripsBinarySafe) {
  const std::string value("a\0\r\n\xff", 5);
  PutString(0, std::string("k\0", 2), value, 0);
  const StoredObject obj = Load(0, std::string("k\0", 2));
  EXPECT_TRUE(obj.IsString());
  EXPECT_EQ(obj.payload, value);
  EXPECT_EQ(obj.meta.expire_ms, 0);
  EXPECT_TRUE(Cf(ColumnFamily::kExpire).empty());
}

TEST_F(ObjectStoreTest, EmptyKeyIsAKey) {
  PutString(0, "", "v", 0);
  EXPECT_TRUE(Load(0, "").IsString());
}

TEST_F(ObjectStoreTest, ExpireIsIndexed) {
  PutString(2, "k", "v", kNow + 5000);
  const auto& idx = Cf(ColumnFamily::kExpire);
  ASSERT_EQ(idx.size(), 1u);
  EXPECT_EQ(idx.begin()->first, ExpireKeyFor(2, kNow + 5000, "k"));
  EXPECT_EQ(idx.begin()->second, ExpireValue(0));
}

TEST_F(ObjectStoreTest, ExpiredAtTheDeadline) {  // Review Focus #1
  PutString(0, "k", "v", kNow + 10);
  EXPECT_TRUE(Load(0, "k", kNow + 9).live());
  EXPECT_EQ(Load(0, "k", kNow + 10).state, ObjectState::kExpired);
}

TEST_F(ObjectStoreTest, ExpiredButStoredIsDeletedByLoadForWrite) {  // Review Focus #1
  PutString(0, "k", "v", kNow + 10);
  Write([&](WriteTxn& t) {
    auto obj = LoadForWrite(t, 0, "k", kNow + 10);
    EXPECT_TRUE(obj.ok());
    EXPECT_EQ(obj->state, ObjectState::kMissing);
    return Status::Ok();
  });
  EXPECT_TRUE(Cf(ColumnFamily::kMeta).empty());
  EXPECT_TRUE(Cf(ColumnFamily::kExpire).empty());
}

TEST_F(ObjectStoreTest, OverwriteMovesTheExpireEntry) {
  PutString(0, "k", "v1", kNow + 5000);
  PutString(0, "k", "v2", kNow + 9000);
  const auto& idx = Cf(ColumnFamily::kExpire);
  ASSERT_EQ(idx.size(), 1u);
  EXPECT_EQ(idx.begin()->first, ExpireKeyFor(0, kNow + 9000, "k"));
}

TEST_F(ObjectStoreTest, OverwriteWithoutExpireDropsTheEntry) {
  PutString(0, "k", "v1", kNow + 5000);
  PutString(0, "k", "v2", 0);
  EXPECT_TRUE(Cf(ColumnFamily::kExpire).empty());
  EXPECT_EQ(Load(0, "k").meta.expire_ms, 0);
}

TEST_F(ObjectStoreTest, MoveObjectMovesMetaAndExpire) {
  PutString(0, "a", "v", kNow + 100);
  Write([&](WriteTxn& t) {
    auto obj = LoadForWrite(t, 0, "a", kNow);
    if (!obj.ok()) return obj.status();
    return MoveObject(t, 0, "a", "b", *obj);
  });
  const auto& meta = Cf(ColumnFamily::kMeta);
  ASSERT_EQ(meta.size(), 1u);
  EXPECT_EQ(meta.begin()->first, MetaKeyFor(0, "b"));
  const StoredObject moved = Load(0, "b");
  EXPECT_TRUE(moved.IsString());
  EXPECT_EQ(moved.payload, "v");
  EXPECT_EQ(moved.meta.expire_ms, kNow + 100);
  EXPECT_EQ(Load(0, "a").state, ObjectState::kMissing);
  const auto& idx = Cf(ColumnFamily::kExpire);
  ASSERT_EQ(idx.size(), 1u);
  EXPECT_EQ(idx.begin()->first, ExpireKeyFor(0, kNow + 100, "b"));
}

TEST_F(ObjectStoreTest, CheckMovableRefusesOnlyCollectionsAcrossSlots) {
  Metadata hash;
  hash.type = ValueType::kHash;
  Metadata str;
  str.type = ValueType::kString;
  ASSERT_NE(KeySlot("h1"), KeySlot("h2"));
  EXPECT_TRUE(CheckMovable(str, "h1", "h2").ok());
  EXPECT_TRUE(CheckMovable(hash, "{t}1", "{t}2").ok());
  EXPECT_FALSE(CheckMovable(hash, "h1", "h2").ok());
}

TEST_F(ObjectStoreTest, DbsAreIsolated) {
  PutString(0, "k", "zero", 0);
  PutString(1, "k", "one", 0);
  EXPECT_EQ(Load(0, "k").payload, "zero");
  EXPECT_EQ(Load(1, "k").payload, "one");
  EXPECT_EQ(Load(2, "k").state, ObjectState::kMissing);
}

void SeedCollection(WriteTxn& t, uint8_t db, const std::string& key, ValueType type, uint64_t oid) {
  Metadata m;
  m.type = type;
  m.oid = oid;
  m.size = 2;
  const uint16_t slot = KeySlot(key);
  ASSERT_TRUE(t.Put(ColumnFamily::kMeta, MetaKeyFor(db, key), encoding::EncodeMetadata(m)).ok());
  ASSERT_TRUE(t.Put(ColumnFamily::kSubkey, encoding::EncodeSubkey(db, slot, oid, "f1"), "x").ok());
  ASSERT_TRUE(t.Put(ColumnFamily::kSubkey, encoding::EncodeSubkey(db, slot, oid, "f2"), "y").ok());
  if (type == ValueType::kZSet) {
    ASSERT_TRUE(t.Put(ColumnFamily::kZScore, encoding::EncodeZScoreKey(db, slot, oid, 1.5, "f1"), "").ok());
  }
}

TEST_F(ObjectStoreTest, DeleteCollectionDropsOnlyItsOwnData) {
  Write([&](WriteTxn& t) {
    SeedCollection(t, 1, "h", ValueType::kHash, 42);
    SeedCollection(t, 1, "other", ValueType::kHash, 43);
    return Status::Ok();
  });
  Write([&](WriteTxn& t) {
    auto obj = LoadForWrite(t, 1, "h", kNow);
    EXPECT_TRUE(obj->live());
    return DeleteObject(t, 1, "h", obj->meta);
  });
  EXPECT_EQ(Load(1, "h").state, ObjectState::kMissing);
  EXPECT_TRUE(Load(1, "other").live());
  EXPECT_EQ(Cf(ColumnFamily::kSubkey).size(), 2u);  // only oid 43's two fields remain
}

TEST_F(ObjectStoreTest, DeleteZSetAlsoDropsScores) {
  Write([&](WriteTxn& t) {
    SeedCollection(t, 0, "z", ValueType::kZSet, 7);
    return Status::Ok();
  });
  Write([&](WriteTxn& t) {
    auto obj = LoadForWrite(t, 0, "z", kNow);
    return DeleteObject(t, 0, "z", obj->meta);
  });
  EXPECT_TRUE(Cf(ColumnFamily::kSubkey).empty());
  EXPECT_TRUE(Cf(ColumnFamily::kZScore).empty());
}

TEST_F(ObjectStoreTest, WriteStringOverCollectionDropsItsData) {
  Write([&](WriteTxn& t) {
    SeedCollection(t, 0, "k", ValueType::kHash, 9);
    return Status::Ok();
  });
  PutString(0, "k", "now a string", 0);
  EXPECT_TRUE(Load(0, "k").IsString());
  EXPECT_TRUE(Cf(ColumnFamily::kSubkey).empty());
}

TEST_F(ObjectStoreTest, SetObjectExpireSetsMovesAndPersists) {
  PutString(0, "k", "v", 0);
  auto set_expire = [&](int64_t when) {
    Write([&](WriteTxn& t) {
      auto obj = LoadForWrite(t, 0, "k", kNow);
      return SetObjectExpire(t, 0, "k", *obj, when);
    });
  };
  set_expire(kNow + 100);
  EXPECT_EQ(Load(0, "k").meta.expire_ms, kNow + 100);
  set_expire(kNow + 200);
  ASSERT_EQ(Cf(ColumnFamily::kExpire).size(), 1u);
  EXPECT_EQ(Cf(ColumnFamily::kExpire).begin()->first, ExpireKeyFor(0, kNow + 200, "k"));
  set_expire(0);
  EXPECT_TRUE(Cf(ColumnFamily::kExpire).empty());
  EXPECT_EQ(Load(0, "k").payload, "v");  // payload preserved through meta rewrites
}

TEST_F(ObjectStoreTest, CorruptMetadataIsCorruption) {
  Write([&](WriteTxn& t) { return t.Put(ColumnFamily::kMeta, MetaKeyFor(0, "bad"), "\x09"); });
  auto view = storage.BeginRead();
  EXPECT_EQ(LoadObject(**view, 0, "bad", kNow).status().code(), StatusCode::kCorruption);
}

TEST_F(ObjectStoreTest, DeleteStaleExpireEntryRemovesOnlyTheIndexEntry) {
  PutString(0, "k", "v", kNow + 50);
  Write([&](WriteTxn& t) { return DeleteStaleExpireEntry(t, 0, "k", kNow + 50); });
  EXPECT_TRUE(Cf(ColumnFamily::kExpire).empty());
  EXPECT_EQ(Cf(ColumnFamily::kMeta).size(), 1u);
}

TEST_F(ObjectStoreTest, PutObjectMetaKeepsTheExpireEntry) {
  // A collection meta with a deadline: rewriting it (size change) must not touch the index.
  encoding::Metadata m;
  m.type = encoding::ValueType::kSet;
  m.oid = 3;
  m.size = 1;
  m.expire_ms = 9000;
  auto txn = storage.BeginWrite();
  ASSERT_TRUE(PutObjectMeta(**txn, 0, "s", m).ok());
  ASSERT_TRUE((*txn)->Put(ColumnFamily::kExpire, encoding::ExpireKeyFor(0, 9000, "s"), ExpireValue(3)).ok());
  m.size = 2;
  ASSERT_TRUE(PutObjectMeta(**txn, 0, "s", m).ok());
  ASSERT_TRUE((*txn)->Commit().ok());
  auto view = storage.BeginRead();
  auto obj = LoadObject(**view, 0, "s", 0);
  ASSERT_TRUE(obj.ok());
  EXPECT_EQ(obj->meta.size, 2u);
  EXPECT_TRUE((*view)->Get(ColumnFamily::kExpire, encoding::ExpireKeyFor(0, 9000, "s")).ok());
}
}  // namespace
}  // namespace tideskv
