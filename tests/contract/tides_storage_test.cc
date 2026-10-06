#include <gtest/gtest.h>

#include "storage/tides_storage.h"
#include "support/temp_dir.h"

namespace tideskv::test {
namespace {

StorageOptions OptionsFor(const TempDir& dir, const std::string& name = "db") {
  StorageOptions o;
  o.path = (dir.path() / name).string();
  o.sync_mode = SyncMode::kNone;
  return o;
}

std::unique_ptr<TidesStorage> MustOpen(const StorageOptions& o) {
  auto s = TidesStorage::Open(o);
  EXPECT_TRUE(s.ok()) << s.status().ToString();
  return s.ok() ? std::move(s).value() : nullptr;
}

TEST(TidesStorage, SecondOpenOfSameDirectoryIsBusyAndSaysLocked) {
  TempDir dir;
  auto first = MustOpen(OptionsFor(dir));
  ASSERT_NE(first, nullptr);
  auto second = TidesStorage::Open(OptionsFor(dir));
  ASSERT_FALSE(second.ok());
  EXPECT_EQ(second.status().code(), StatusCode::kBusy);
  EXPECT_NE(second.status().message().find("locked"), std::string::npos);
}

TEST(TidesStorage, EveryColumnFamilySurvivesReopen) {
  TempDir dir;
  {
    auto s = MustOpen(OptionsFor(dir));
    ASSERT_NE(s, nullptr);
    auto txn = s->BeginWrite();
    ASSERT_TRUE(txn.ok());
    for (ColumnFamily cf : kAllColumnFamilies) {
      ASSERT_TRUE((*txn)->Put(cf, "k", std::string(ColumnFamilyName(cf))).ok());
    }
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  auto s = MustOpen(OptionsFor(dir));
  ASSERT_NE(s, nullptr);
  auto view = s->BeginRead();
  ASSERT_TRUE(view.ok());
  for (ColumnFamily cf : kAllColumnFamilies) {
    auto v = (*view)->Get(cf, "k");
    ASSERT_TRUE(v.ok()) << ColumnFamilyName(cf);
    EXPECT_EQ(*v, ColumnFamilyName(cf));
  }
}

TEST(TidesStorage, UncommittedWritesAreGoneAfterReopen) {
  TempDir dir;
  {
    auto s = MustOpen(OptionsFor(dir));
    ASSERT_NE(s, nullptr);
    auto txn = s->BeginWrite();
    ASSERT_TRUE(txn.ok());
    ASSERT_TRUE((*txn)->Put(ColumnFamily::kMeta, "k", "v").ok());
  }  // txn destroyed (rolled back) before the storage closes
  auto s = MustOpen(OptionsFor(dir));
  ASSERT_NE(s, nullptr);
  auto view = s->BeginRead();
  ASSERT_TRUE(view.ok());
  EXPECT_TRUE((*view)->Get(ColumnFamily::kMeta, "k").status().IsNotFound());
}

TEST(TidesStorage, BackupIsDirectlyOpenable) {
  TempDir dir;
  auto s = MustOpen(OptionsFor(dir));
  ASSERT_NE(s, nullptr);
  {
    auto txn = s->BeginWrite();
    ASSERT_TRUE(txn.ok());
    ASSERT_TRUE((*txn)->Put(ColumnFamily::kSubkey, "field", "value").ok());
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  const std::string backup_dir = (dir.path() / "backup").string();
  ASSERT_TRUE(s->Backup(backup_dir).ok());
  StorageOptions bo = OptionsFor(dir, "backup");
  auto copy = MustOpen(bo);
  ASSERT_NE(copy, nullptr);
  auto view = copy->BeginRead();
  ASSERT_TRUE(view.ok());
  auto v = (*view)->Get(ColumnFamily::kSubkey, "field");
  ASSERT_TRUE(v.ok());
  EXPECT_EQ(*v, "value");
}

TEST(TidesStorage, CheckpointSucceeds) {
  TempDir dir;
  auto s = MustOpen(OptionsFor(dir));
  ASSERT_NE(s, nullptr);
  EXPECT_TRUE(s->Checkpoint().ok());
}

}  // namespace
}  // namespace tideskv::test
