#include <gtest/gtest.h>

#include "storage/tides_storage.h"
#include "support/temp_dir.h"
#include "types/oid_allocator.h"

namespace tideskv {
namespace {
using test::TempDir;

StorageOptions Opts(const TempDir& dir) {
  StorageOptions o;
  o.path = (dir.path() / "db").string();
  o.sync_mode = SyncMode::kNone;
  return o;
}

TEST(OidAllocatorTides, LeaseInsideAnOpenTransactionSurvivesReopen) {
  test::TempDir dir;
  uint64_t first = 0;
  {
    auto s = TidesStorage::Open(Opts(dir));
    ASSERT_TRUE(s.ok()) << s.status().ToString();
    OidAllocator a(**s, [&] { return (*s)->Checkpoint(); });
    ASSERT_TRUE(a.Init().ok());
    auto txn = (*s)->BeginWrite();  // a command's transaction is open while it leases
    ASSERT_TRUE(txn.ok());
    auto id = a.Next();
    ASSERT_TRUE(id.ok()) << id.status().ToString();
    first = *id;
    ASSERT_TRUE((*txn)->Put(ColumnFamily::kSubkey, "x", "y").ok());
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  auto s = TidesStorage::Open(Opts(dir));
  ASSERT_TRUE(s.ok());
  OidAllocator b(**s);
  ASSERT_TRUE(b.Init().ok());
  EXPECT_EQ(*b.Next(), first + OidAllocator::kLeaseSize);
}

}  // namespace
}  // namespace tideskv
