#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include "encoding/key_codec.h"
#include "exec/engine.h"
#include "exec/write_observer.h"
#include "storage/memory_storage.h"
#include "support/fake_clock.h"

namespace tideskv {
namespace {

using test::FakeClock;

class Observer final : public WriteObserver {
 public:
  bool Idle() const override { return false; }
  void OnModified(const std::vector<DbKey>& keys, const CommittedViewFn&) override {
    reported.insert(reported.end(), keys.begin(), keys.end());
  }
  std::vector<std::string> ExistingWatched(uint8_t, ReadView&) override { return {}; }
  std::vector<DbKey> reported;
};

struct BatchTest : ::testing::Test {
  InMemoryStorage storage;
  LockManager locks;
  FakeClock clock{1000};
  Observer observer;
  Engine engine{storage, locks, clock, nullptr, &observer};

  Status Put(uint8_t db, const std::string& key, const std::string& v) {
    return engine.Write(db, {key}, [&](WriteTxn& t, int64_t) {
      return t.Put(ColumnFamily::kMeta, encoding::MetaKeyFor(db, key), v);
    });
  }
  std::string Get(uint8_t db, const std::string& key) {
    std::string out = "<missing>";
    EXPECT_TRUE(engine
                    .Read([&](ReadView& v, int64_t) {
                      auto r = v.Get(ColumnFamily::kMeta, encoding::MetaKeyFor(db, key));
                      if (r.ok()) out = *r;
                      return r.ok() || r.status().IsNotFound() ? Status::Ok() : r.status();
                    })
                    .ok());
    return out;
  }
};

TEST_F(BatchTest, WritesAreInvisibleToOthersUntilCommit) {
  auto batch = engine.BeginBatch();
  ASSERT_TRUE(batch.ok());
  EXPECT_TRUE(engine.InBatch());
  ASSERT_TRUE(Put(0, "k", "v").ok());
  EXPECT_EQ(Get(0, "k"), "v");  // read-your-writes inside the batch
  std::string outside;
  std::thread other([&] {  // another thread waits for the gate, then sees the committed value
    outside = Get(0, "k");
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  ASSERT_TRUE((*batch)->Commit().ok());
  EXPECT_FALSE(engine.InBatch());
  batch->reset();  // releases the gate
  other.join();
  EXPECT_EQ(outside, "v");
}

TEST_F(BatchTest, DestroyingWithoutCommitRollsBack) {
  {
    auto batch = engine.BeginBatch();
    ASSERT_TRUE(batch.ok());
    ASSERT_TRUE(Put(0, "k", "v").ok());
  }
  EXPECT_FALSE(engine.InBatch());
  EXPECT_EQ(Get(0, "k"), "<missing>");
  EXPECT_TRUE(observer.reported.empty());
}

TEST_F(BatchTest, FailedWriteIsUndoneButTheBatchGoesOn) {
  auto batch = engine.BeginBatch();
  ASSERT_TRUE(batch.ok());
  ASSERT_TRUE(Put(0, "a", "1").ok());
  EXPECT_EQ(engine
                .Write(0, {"b"},
                       [](WriteTxn& t, int64_t) {
                         EXPECT_TRUE(t.Put(ColumnFamily::kMeta, encoding::MetaKeyFor(0, "b"), "half").ok());
                         return Status::UserError("WRONGTYPE x");
                       })
                .code(),
            StatusCode::kUserError);
  ASSERT_TRUE(Put(0, "c", "3").ok());
  ASSERT_TRUE((*batch)->Commit().ok());
  batch->reset();
  EXPECT_EQ(Get(0, "a"), "1");
  EXPECT_EQ(Get(0, "b"), "<missing>");
  EXPECT_EQ(Get(0, "c"), "3");
  EXPECT_EQ(observer.reported, (std::vector<DbKey>{{0, "a"}, {0, "c"}}));  // after commit only
}

TEST_F(BatchTest, ClockIsFrozen) {
  auto batch = engine.BeginBatch();
  ASSERT_TRUE(batch.ok());
  clock.Advance(500);
  EXPECT_EQ((*batch)->now_ms(), 1000);
  EXPECT_EQ(engine.NowMs(), 1000);
  int64_t seen = 0;
  ASSERT_TRUE(engine
                  .Write(0, {"k"},
                         [&](WriteTxn&, int64_t now) {
                           seen = now;
                           return Status::Ok();
                         })
                  .ok());
  EXPECT_EQ(seen, 1000);
  batch->reset();
  EXPECT_EQ(engine.NowMs(), 1500);
}

TEST_F(BatchTest, ExclusiveWriteAndSnapshotRunInsideTheBatch) {
  ASSERT_TRUE(Put(0, "old", "x").ok());
  auto batch = engine.BeginBatch();
  ASSERT_TRUE(batch.ok());
  ASSERT_TRUE(
      engine
          .WriteExclusive(
              [](WriteTxn& t, int64_t) { return t.DeletePrefix(ColumnFamily::kMeta, encoding::DbPrefix(0)); },
              {0})
          .ok());
  ASSERT_TRUE(Put(0, "new", "y").ok());
  ASSERT_TRUE(engine
                  .Snapshot([&](ReadView& v, int64_t) {
                    EXPECT_FALSE(v.Get(ColumnFamily::kMeta, encoding::MetaKeyFor(0, "old")).ok());
                    EXPECT_TRUE(v.Get(ColumnFamily::kMeta, encoding::MetaKeyFor(0, "new")).ok());
                    return Status::Ok();
                  })
                  .ok());
  ASSERT_TRUE((*batch)->Commit().ok());
}

TEST_F(BatchTest, OtherWritersWaitForTheBatch) {
  auto batch = engine.BeginBatch();
  ASSERT_TRUE(batch.ok());
  std::atomic<bool> done{false};
  std::thread writer([&] {
    EXPECT_TRUE(Put(0, "w", "1").ok());
    done = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(done.load());  // blocked on the gate
  batch->reset();
  writer.join();
  EXPECT_TRUE(done.load());
}

TEST_F(BatchTest, NestedBatchIsRefused) {
  auto batch = engine.BeginBatch();
  ASSERT_TRUE(batch.ok());
  auto nested = engine.BeginBatch();
  EXPECT_FALSE(nested.ok());
  EXPECT_EQ(nested.status().code(), StatusCode::kInternal);
}

TEST_F(BatchTest, SecondCommitFails) {
  auto batch = engine.BeginBatch();
  ASSERT_TRUE(batch.ok());
  ASSERT_TRUE((*batch)->Commit().ok());
  EXPECT_EQ((*batch)->Commit().code(), StatusCode::kInternal);
}

// Review Focus #5: a queued handler that throws unwinds through the batch.
TEST_F(BatchTest, ExceptionRollsBackUnroutesAndReleasesTheGate) {
  try {
    auto batch = engine.BeginBatch();
    ASSERT_TRUE(batch.ok());
    ASSERT_TRUE(Put(0, "k", "v").ok());
    throw std::runtime_error("boom");
  } catch (const std::runtime_error&) {
  }
  EXPECT_FALSE(engine.InBatch());
  EXPECT_EQ(Get(0, "k"), "<missing>");
  EXPECT_TRUE(Put(0, "k", "w").ok());  // the gate is free again
}

TEST_F(BatchTest, ThrowingWriteIsUndoneButTheBatchGoesOn) {
  auto batch = engine.BeginBatch();
  ASSERT_TRUE(batch.ok());
  EXPECT_THROW(
      (void)engine.Write(0, {"b"},
                         [](WriteTxn& t, int64_t) -> Status {
                           EXPECT_TRUE(t.Put(ColumnFamily::kMeta, encoding::MetaKeyFor(0, "b"), "half").ok());
                           throw std::runtime_error("boom");
                         }),
      std::runtime_error);
  ASSERT_TRUE(Put(0, "c", "3").ok());
  ASSERT_TRUE((*batch)->Commit().ok());
  batch->reset();
  EXPECT_EQ(Get(0, "b"), "<missing>");
  EXPECT_EQ(Get(0, "c"), "3");
}

TEST_F(BatchTest, ThousandCommandsWithFailuresInterleaved) {
  auto batch = engine.BeginBatch();
  ASSERT_TRUE(batch.ok());
  for (int i = 0; i < 1000; ++i) {
    const std::string k = "k" + std::to_string(i);
    const Status st = engine.Write(0, {k}, [&](WriteTxn& t, int64_t) {
      EXPECT_TRUE(t.Put(ColumnFamily::kMeta, encoding::MetaKeyFor(0, k), "v").ok());
      return i % 3 == 0 ? Status::UserError("ERR x") : Status::Ok();
    });
    EXPECT_EQ(st.ok(), i % 3 != 0);
  }
  ASSERT_TRUE((*batch)->Commit().ok());
  batch->reset();
  EXPECT_EQ(Get(0, "k0"), "<missing>");
  EXPECT_EQ(Get(0, "k1"), "v");
  EXPECT_EQ(observer.reported.size(), 666u);
}

}  // namespace
}  // namespace tideskv
