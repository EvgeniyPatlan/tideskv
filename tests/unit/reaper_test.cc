#include "expire/reaper.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>

#include "common/crc16.h"
#include "encoding/key_codec.h"
#include "exec/engine.h"
#include "exec/lock_manager.h"
#include "support/command_harness.h"
#include "types/object_store.h"

namespace tideskv {
namespace {

using test::CommandHarness;
using test::kOk;

ExpireReaper::Options Fast() {
  ExpireReaper::Options o;
  o.interval = std::chrono::milliseconds(5);
  o.cycle_budget = std::chrono::seconds(10);
  return o;
}

// Forwards to a real storage, but the first `throws` BeginRead calls throw, as a bad_alloc
// or a bug below Engine would.
class ThrowingStorage final : public Storage {
 public:
  ThrowingStorage(Storage& inner, int throws) : inner_(inner), throws_left_(throws) {}
  Result<std::unique_ptr<WriteTxn>> BeginWrite() override { return inner_.BeginWrite(); }
  Result<std::unique_ptr<ReadView>> BeginRead() override {
    if (throws_left_.fetch_sub(1) > 0) throw std::runtime_error("injected failure");
    return inner_.BeginRead();
  }
  Result<std::unique_ptr<ReadView>> BeginSnapshot() override { return inner_.BeginSnapshot(); }

 private:
  Storage& inner_;
  std::atomic<int> throws_left_;
};

struct ReaperTest : ::testing::Test {
  CommandHarness h;
  ExpireReaper reaper{h.services.engine_, Fast()};
  std::string Run(const ArgList& a) { return h.Run(a); }
};

TEST_F(ReaperTest, DeletesOnlyDueKeys) {
  EXPECT_EQ(Run({"SET", "soon", "v", "PX", "10"}), kOk);
  EXPECT_EQ(Run({"SET", "later", "v", "PX", "1000"}), kOk);
  EXPECT_EQ(Run({"SET", "never", "v"}), kOk);
  EXPECT_EQ(*reaper.RunOnce(), 0u);
  h.clock().Advance(10);
  EXPECT_EQ(*reaper.RunOnce(), 1u);
  EXPECT_EQ(h.Cf(ColumnFamily::kMeta).size(), 2u);
  EXPECT_EQ(h.Cf(ColumnFamily::kExpire).size(), 1u);
  EXPECT_EQ(reaper.expired_total(), 1u);
}

TEST_F(ReaperTest, CoversEveryDb) {
  for (uint8_t db : {0, 7, 15}) {
    h.session.db = db;
    EXPECT_EQ(Run({"SET", "k", "v", "PX", "1"}), kOk);
  }
  h.clock().Advance(1);
  EXPECT_EQ(*reaper.RunOnce(), 3u);
  EXPECT_TRUE(h.Cf(ColumnFamily::kMeta).empty());
}

TEST_F(ReaperTest, StaleEntryIsDroppedButKeyKept) {
  EXPECT_EQ(Run({"SET", "k", "v", "PX", "1000"}), kOk);
  {  // plant a stale entry for an older deadline, as a crash between steps could leave behind
    auto txn = h.services.storage_.BeginWrite();
    ASSERT_TRUE((*txn)
                    ->Put(ColumnFamily::kExpire, encoding::ExpireKeyFor(0, h.clock().NowMs() + 5, "k"),
                          ExpireValue(0))
                    .ok());
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  h.clock().Advance(5);
  EXPECT_EQ(*reaper.RunOnce(), 0u);
  EXPECT_EQ(Run({"GET", "k"}), test::Bulk("v"));
  EXPECT_EQ(h.Cf(ColumnFamily::kExpire).size(), 1u);  // only the real entry is left
}

TEST_F(ReaperTest, DeletesCollectionsWithTheirData) {
  encoding::Metadata m;
  m.type = encoding::ValueType::kHash;
  m.oid = 9;
  m.expire_ms = h.clock().NowMs() + 1;
  h.SeedMeta(0, "h", m);
  auto txn = h.services.storage_.BeginWrite();
  ASSERT_TRUE((*txn)->Put(ColumnFamily::kSubkey, encoding::EncodeSubkey(0, KeySlot("h"), 9, "f"), "v").ok());
  ASSERT_TRUE((*txn)->Commit().ok());
  h.clock().Advance(1);
  EXPECT_EQ(*reaper.RunOnce(), 1u);
  EXPECT_TRUE(h.Cf(ColumnFamily::kSubkey).empty());
}

TEST_F(ReaperTest, LargeBacklogDrainsInOneCycleWithinBudget) {
  for (int i = 0; i < 2500; ++i) h.Run({"SET", "k" + std::to_string(i), "v", "PX", "1"});
  h.clock().Advance(1);
  EXPECT_EQ(*reaper.RunOnce(), 2500u);
}

TEST_F(ReaperTest, ZeroBudgetStillMakesProgress) {
  ExpireReaper::Options o = Fast();
  o.cycle_budget = std::chrono::milliseconds(0);
  ExpireReaper tight(h.services.engine_, o);
  for (int i = 0; i < 2500; ++i) h.Run({"SET", "k" + std::to_string(i), "v", "PX", "1"});
  h.clock().Advance(1);
  const size_t first = *tight.RunOnce();
  EXPECT_GE(first, 1000u);  // at least one full batch
  EXPECT_LT(first, 2500u);
}

TEST_F(ReaperTest, BackgroundThreadReapsAndStops) {
  for (int i = 0; i < 10; ++i) h.Run({"SET", "k" + std::to_string(i), "v", "PX", "1"});
  h.clock().Advance(1);
  reaper.Start();
  for (int i = 0; i < 200 && reaper.expired_total() < 10; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  reaper.Stop();
  reaper.Stop();  // idempotent
  EXPECT_EQ(reaper.expired_total(), 10u);
}

TEST_F(ReaperTest, BogusEntryNeverDeletesPersistentKey) {
  EXPECT_EQ(Run({"SET", "k", "v"}), kOk);
  {  // an index entry that does not match meta (expire_ms 0, oid 0), e.g. from a bug
    auto txn = h.services.storage_.BeginWrite();
    ASSERT_TRUE((*txn)->Put(ColumnFamily::kExpire, encoding::ExpireKeyFor(0, 0, "k"), ExpireValue(0)).ok());
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  EXPECT_EQ(*reaper.RunOnce(), 0u);
  EXPECT_EQ(Run({"GET", "k"}), test::Bulk("v"));
  EXPECT_TRUE(h.Cf(ColumnFamily::kExpire).empty());  // the bogus entry is dropped
}

TEST_F(ReaperTest, UndecodableEntryIsSkippedAndLaterDbsAreStillReaped) {
  {  // db 0: a due entry whose value is not an 8-byte oid; it must not end the cycle
    auto txn = h.services.storage_.BeginWrite();
    ASSERT_TRUE((*txn)->Put(ColumnFamily::kExpire, encoding::ExpireKeyFor(0, 1, "bad"), "xyz").ok());
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  h.session.db = 1;
  EXPECT_EQ(Run({"SET", "k", "v", "PX", "1"}), kOk);
  h.clock().Advance(1);
  auto n = reaper.RunOnce();
  ASSERT_TRUE(n.ok()) << n.status().ToString();
  EXPECT_EQ(*n, 1u);
  EXPECT_EQ(Run({"EXISTS", "k"}), test::Int(0));
  EXPECT_EQ(h.Cf(ColumnFamily::kMeta).size(), 0u);
}

TEST_F(ReaperTest, ThreadSurvivesAnException) {
  ThrowingStorage storage(h.services.storage_, /*throws=*/1);
  LockManager locks;
  Engine engine(storage, locks, h.clock());
  ExpireReaper survivor(engine, Fast());
  EXPECT_EQ(Run({"SET", "k", "v", "PX", "1"}), kOk);
  h.clock().Advance(1);
  survivor.Start();  // the first pass throws; the next one must still reap
  for (int i = 0; i < 200 && survivor.expired_total() < 1; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  survivor.Stop();
  EXPECT_EQ(survivor.expired_total(), 1u);
}

TEST_F(ReaperTest, ReapsExpiredCollectionsWithTheirElements) {
  EXPECT_EQ(h.Run({"HSET", "h", "a", "1", "b", "2"}), test::Int(2));
  EXPECT_EQ(h.Run({"SADD", "s", "m"}), test::Int(1));
  EXPECT_EQ(h.Run({"PEXPIRE", "h", "10"}), test::Int(1));
  EXPECT_EQ(h.Run({"PEXPIRE", "s", "10"}), test::Int(1));
  h.clock().Advance(10);
  auto n = reaper.RunOnce();
  ASSERT_TRUE(n.ok());
  EXPECT_EQ(*n, 2u);
  EXPECT_TRUE(h.Cf(ColumnFamily::kMeta).empty());
  EXPECT_TRUE(h.Cf(ColumnFamily::kExpire).empty());
  EXPECT_TRUE(h.Cf(ColumnFamily::kSubkey).empty());
}

TEST_F(ReaperTest, RecreatedCollectionWithANewDeadlineIsNotReaped) {
  EXPECT_EQ(h.Run({"SADD", "s", "old"}), test::Int(1));
  EXPECT_EQ(h.Run({"PEXPIRE", "s", "10"}), test::Int(1));
  h.clock().Advance(10);
  EXPECT_EQ(h.Run({"SADD", "s", "new"}), test::Int(1));  // drops the expired set, new oid
  EXPECT_EQ(h.Run({"PEXPIRE", "s", "100000"}), test::Int(1));
  ASSERT_TRUE(reaper.RunOnce().ok());
  EXPECT_EQ(h.Run({"SMEMBERS", "s"}), test::Arr({test::Bulk("new")}));
}

// A stale expire entry left by an earlier incarnation (old oid) with the same deadline as the
// recreated collection, which is itself expired: only the oid tells them apart.
TEST_F(ReaperTest, StaleEntryOfAnOlderOidDeletesNothing) {
  constexpr uint64_t kOldOid = 41;
  constexpr uint64_t kNewOid = 42;
  const int64_t deadline = h.clock().NowMs() + 10;
  encoding::Metadata meta;
  meta.type = encoding::ValueType::kSet;
  meta.oid = kNewOid;
  meta.size = 1;
  meta.expire_ms = deadline;
  h.SeedMeta(0, "s", meta);
  {
    auto txn = h.services.storage_.BeginWrite();
    ASSERT_TRUE(txn.ok());
    ASSERT_TRUE(
        (*txn)->Put(ColumnFamily::kSubkey, encoding::EncodeSubkey(0, KeySlot("s"), kNewOid, "m"), "").ok());
    ASSERT_TRUE((*txn)
                    ->Put(ColumnFamily::kExpire,
                          encoding::ExpireKeyFor(0, static_cast<uint64_t>(deadline), "s"),
                          ExpireValue(kOldOid))
                    .ok());
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  h.clock().Advance(10);
  EXPECT_EQ(*reaper.RunOnce(), 0u);
  EXPECT_EQ(reaper.expired_total(), 0u);
  EXPECT_EQ(h.Cf(ColumnFamily::kMeta).size(), 1u);  // the collection was not deleted
  EXPECT_EQ(h.Cf(ColumnFamily::kSubkey).size(), 1u);
  EXPECT_EQ(h.Cf(ColumnFamily::kExpire).size(), 0u);  // the stale entry was dropped
}

}  // namespace
}  // namespace tideskv
