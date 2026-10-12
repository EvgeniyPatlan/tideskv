#include "types/oid_allocator.h"

#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <thread>
#include <vector>

#include "common/coding.h"
#include "storage/memory_storage.h"

namespace tideskv {
namespace {

uint64_t StoredHwm(InMemoryStorage& s) {
  auto view = s.BeginRead();
  EXPECT_TRUE(view.ok());
  auto raw = (*view)->Get(ColumnFamily::kSys, OidAllocator::kHwmKey);
  if (!raw.ok()) return 0;
  std::string_view in = *raw;
  uint64_t v = 0;
  EXPECT_TRUE(coding::GetFixed64BE(&in, &v));
  return v;
}

TEST(OidAllocator, FreshStoreStartsAtOneAndLeasesABlock) {
  InMemoryStorage s;
  int persisted = 0;
  OidAllocator a(s, [&] {
    ++persisted;
    return Status::Ok();
  });
  ASSERT_TRUE(a.Init().ok());
  auto first = a.Next();
  ASSERT_TRUE(first.ok());
  EXPECT_EQ(*first, 1u);  // oid 0 is reserved for strings
  EXPECT_EQ(StoredHwm(s), 1 + OidAllocator::kLeaseSize);
  EXPECT_EQ(persisted, 1);
  EXPECT_EQ(*a.Next(), 2u);
  EXPECT_EQ(persisted, 1);  // same lease: no new write
}

TEST(OidAllocator, NextLeaseIsWrittenWhenTheBlockRunsOut) {
  InMemoryStorage s;
  OidAllocator a(s);
  ASSERT_TRUE(a.Init().ok());
  uint64_t last = 0;
  for (uint64_t i = 0; i < OidAllocator::kLeaseSize + 1; ++i) last = *a.Next();
  EXPECT_EQ(last, OidAllocator::kLeaseSize + 1);
  EXPECT_EQ(StoredHwm(s), 1 + 2 * OidAllocator::kLeaseSize);
}

TEST(OidAllocator, RestartSkipsTheRestOfTheLease) {
  InMemoryStorage s;
  {
    OidAllocator a(s);
    ASSERT_TRUE(a.Init().ok());
    EXPECT_EQ(*a.Next(), 1u);
  }
  OidAllocator b(s);
  ASSERT_TRUE(b.Init().ok());
  EXPECT_EQ(*b.Next(), 1 + OidAllocator::kLeaseSize);  // never reuses 2..65536
}

TEST(OidAllocator, NextBeforeInitIsAnInternalError) {
  InMemoryStorage s;
  OidAllocator a(s);
  EXPECT_EQ(a.Next().status().code(), StatusCode::kInternal);
}

TEST(OidAllocator, CorruptHwmFailsInit) {
  InMemoryStorage s;
  auto txn = s.BeginWrite();
  ASSERT_TRUE((*txn)->Put(ColumnFamily::kSys, OidAllocator::kHwmKey, "bad").ok());
  ASSERT_TRUE((*txn)->Commit().ok());
  OidAllocator a(s);
  EXPECT_EQ(a.Init().code(), StatusCode::kCorruption);
}

TEST(OidAllocator, PersistFailureIsReturnedAndRetried) {
  InMemoryStorage s;
  bool fail = true;
  OidAllocator a(s, [&] { return fail ? Status::IOError("disk") : Status::Ok(); });
  ASSERT_TRUE(a.Init().ok());
  EXPECT_EQ(a.Next().status().code(), StatusCode::kIOError);
  fail = false;
  auto id = a.Next();
  ASSERT_TRUE(id.ok());
  EXPECT_EQ(*id, 1u);
  EXPECT_EQ(a.hwm(), 1 + OidAllocator::kLeaseSize);
}

TEST(OidAllocator, ConcurrentCallersGetDistinctIds) {
  InMemoryStorage s;
  OidAllocator a(s);
  ASSERT_TRUE(a.Init().ok());
  std::vector<std::vector<uint64_t>> got(4);
  std::atomic<bool> failed{false};
  std::vector<std::thread> threads;
  threads.reserve(4);
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 20000; ++i) {
        auto id = a.Next();
        if (!id.ok()) {
          failed = true;
          return;
        }
        got[t].push_back(*id);
      }
    });
  }
  for (auto& th : threads) th.join();
  ASSERT_FALSE(failed);
  std::set<uint64_t> all;
  for (const auto& v : got) all.insert(v.begin(), v.end());
  EXPECT_EQ(all.size(), 80000u);
}

}  // namespace
}  // namespace tideskv
