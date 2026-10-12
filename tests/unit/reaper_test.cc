#include "expire/reaper.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "common/crc16.h"
#include "encoding/key_codec.h"
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

}  // namespace
}  // namespace tideskv
