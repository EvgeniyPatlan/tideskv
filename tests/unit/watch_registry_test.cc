#include "server/watch.h"

#include <gtest/gtest.h>

#include "encoding/key_codec.h"
#include "storage/memory_storage.h"

namespace tideskv {
namespace {

struct WatchRegistryTest : ::testing::Test {
  InMemoryStorage storage;
  WatchRegistry reg;
  std::shared_ptr<WatchClient> a = std::make_shared<WatchClient>();
  std::shared_ptr<WatchClient> b = std::make_shared<WatchClient>();

  void Create(uint8_t db, const std::string& key) {
    auto t = storage.BeginWrite();
    ASSERT_TRUE((*t)->Put(ColumnFamily::kMeta, encoding::MetaKeyFor(db, key), "m").ok());
    ASSERT_TRUE((*t)->Commit().ok());
  }
  void Touch(uint8_t db, const std::string& key) {
    auto v = storage.BeginRead();
    reg.OnModified({{db, key}}, v->get());
  }
};

TEST_F(WatchRegistryTest, IdleUntilSomethingIsWatched) {
  EXPECT_TRUE(reg.Idle());
  reg.Watch(a, 0, "k", false);
  EXPECT_FALSE(reg.Idle());
  EXPECT_EQ(reg.size(), 1u);
  reg.UnwatchAll(*a);
  EXPECT_TRUE(reg.Idle());
}

TEST_F(WatchRegistryTest, TouchDirtiesEveryWatcherOfThatDbAndKeyOnly) {
  reg.Watch(a, 0, "k", false);
  reg.Watch(b, 1, "k", false);
  Touch(0, "other");
  Touch(2, "k");
  EXPECT_FALSE(a->dirty());
  Touch(0, "k");
  EXPECT_TRUE(a->dirty());
  EXPECT_FALSE(b->dirty());
}

TEST_F(WatchRegistryTest, DuplicateWatchIsIgnored) {
  reg.Watch(a, 0, "k", false);
  reg.Watch(a, 0, "k", false);
  EXPECT_EQ(reg.size(), 1u);
  EXPECT_EQ(a->keys().size(), 1u);
}

TEST_F(WatchRegistryTest, DirtyClientRegistersNothing) {
  reg.Watch(a, 0, "k", false);
  Touch(0, "k");
  reg.Watch(a, 0, "x", false);
  EXPECT_EQ(a->keys().size(), 1u);
}

TEST_F(WatchRegistryTest, UnwatchAllClearsDirtyAndKeys) {
  reg.Watch(a, 0, "k", false);
  Touch(0, "k");
  reg.UnwatchAll(*a);
  EXPECT_FALSE(a->dirty());
  EXPECT_TRUE(a->keys().empty());
  Touch(0, "k");
  EXPECT_FALSE(a->dirty());
}

// Redis multi.c:393-402: deleting a key that was already expired at WATCH time is no change;
// recreating it is.
TEST_F(WatchRegistryTest, ExpiredAtWatchIsClearedByDeletionAndDirtiedByRecreation) {
  reg.Watch(a, 0, "k", true);
  Touch(0, "k");  // the key no longer exists: flag cleared, not dirty
  EXPECT_FALSE(a->dirty());
  EXPECT_EQ(reg.KeysWatchedLive(*a), (std::vector<DbKey>{{0, "k"}}));
  Create(0, "k");
  Touch(0, "k");
  EXPECT_TRUE(a->dirty());
}

TEST_F(WatchRegistryTest, ExpiredAtWatchAndStillThereIsDirty) {
  Create(0, "k");
  reg.Watch(a, 0, "k", true);
  EXPECT_TRUE(reg.KeysWatchedLive(*a).empty());
  Touch(0, "k");
  EXPECT_TRUE(a->dirty());
}

// A recreate landing between registration and the expired flag must dirty the client.
TEST_F(WatchRegistryTest, WriteBetweenRegistrationAndExpiredFlagDirties) {
  reg.Watch(a, 0, "k", false);
  Create(0, "k");
  Touch(0, "k");
  reg.MarkExpiredAtWatch(*a, 0, "k");
  EXPECT_TRUE(a->dirty());
}

TEST_F(WatchRegistryTest, ExpiredFlagSetAfterRegistrationKeepsStaleKeySemantics) {
  Create(0, "k");
  reg.Watch(a, 0, "k", false);
  reg.MarkExpiredAtWatch(*a, 0, "k");
  EXPECT_TRUE(reg.KeysWatchedLive(*a).empty());
  auto t = storage.BeginWrite();
  ASSERT_TRUE((*t)->Delete(ColumnFamily::kMeta, encoding::MetaKeyFor(0, "k")).ok());
  ASSERT_TRUE((*t)->Commit().ok());
  Touch(0, "k");
  EXPECT_FALSE(a->dirty());
  EXPECT_FALSE(reg.KeysWatchedLive(*a).empty());
}

TEST_F(WatchRegistryTest, NullViewCountsAsExisting) {
  reg.Watch(a, 0, "k", true);
  reg.OnModified({{0, "k"}}, nullptr);
  EXPECT_TRUE(a->dirty());
}

TEST_F(WatchRegistryTest, ExistingWatchedListsOnlyExistingKeysOfThatDb) {
  Create(0, "here");
  Create(1, "gone");
  reg.Watch(a, 0, "here", false);
  reg.Watch(a, 0, "absent", false);
  reg.Watch(b, 1, "here", false);
  auto v = storage.BeginRead();
  EXPECT_EQ(reg.ExistingWatched(0, **v), (std::vector<std::string>{"here"}));
  EXPECT_TRUE(reg.ExistingWatched(1, **v).empty());
}

}  // namespace
}  // namespace tideskv
