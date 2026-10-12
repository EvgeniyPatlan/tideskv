#include "exec/engine.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "storage/memory_storage.h"
#include "support/fake_clock.h"

namespace tideskv {
namespace {

using test::FakeClock;

// Forwards to a real transaction but fails the first `failures` commits with Conflict.
class ConflictingTxn final : public WriteTxn {
 public:
  ConflictingTxn(std::unique_ptr<WriteTxn> inner, std::atomic<int>* failures)
      : inner_(std::move(inner)), failures_(failures) {}
  Result<std::string> Get(ColumnFamily cf, std::string_view k) override { return inner_->Get(cf, k); }
  Result<std::unique_ptr<Iterator>> NewIterator(ColumnFamily cf, IterOptions o) override {
    return inner_->NewIterator(cf, std::move(o));
  }
  Status Put(ColumnFamily cf, std::string_view k, std::string_view v) override {
    return inner_->Put(cf, k, v);
  }
  Status Delete(ColumnFamily cf, std::string_view k) override { return inner_->Delete(cf, k); }
  Status DeletePrefix(ColumnFamily cf, std::string_view p) override { return inner_->DeletePrefix(cf, p); }
  Status DeleteRange(ColumnFamily cf, std::string_view lo, std::string_view hi) override {
    return inner_->DeleteRange(cf, lo, hi);
  }
  Status Savepoint(std::string_view n) override { return inner_->Savepoint(n); }
  Status RollbackToSavepoint(std::string_view n) override { return inner_->RollbackToSavepoint(n); }
  Status Commit() override {
    if (failures_->fetch_sub(1) > 0) return Status::Conflict("injected");
    return inner_->Commit();
  }
  Status Rollback() override { return inner_->Rollback(); }

 private:
  std::unique_ptr<WriteTxn> inner_;
  std::atomic<int>* failures_;
};

class ConflictingStorage final : public Storage {
 public:
  explicit ConflictingStorage(int failures) : failures_(failures) {}
  Result<std::unique_ptr<WriteTxn>> BeginWrite() override {
    auto txn = inner_.BeginWrite();
    if (!txn.ok()) return txn.status();
    return std::unique_ptr<WriteTxn>(std::make_unique<ConflictingTxn>(std::move(txn).value(), &failures_));
  }
  Result<std::unique_ptr<ReadView>> BeginRead() override { return inner_.BeginRead(); }
  Result<std::unique_ptr<ReadView>> BeginSnapshot() override { return inner_.BeginSnapshot(); }

 private:
  InMemoryStorage inner_;
  std::atomic<int> failures_;
};

std::string ReadMeta(Engine& e, const std::string& key) {
  std::string out = "<missing>";
  EXPECT_TRUE(e.Read([&](ReadView& v, int64_t) {
                 auto r = v.Get(ColumnFamily::kMeta, key);
                 if (r.ok()) out = *r;
                 return r.ok() || r.status().IsNotFound() ? Status::Ok() : r.status();
               }).ok());
  return out;
}

struct EngineFixture : ::testing::Test {
  InMemoryStorage storage;
  LockManager locks;
  FakeClock clock;
  Engine engine{storage, locks, clock};
};

TEST_F(EngineFixture, NowComesFromTheClock) {
  clock.Set(42);
  EXPECT_EQ(engine.NowMs(), 42);
}

TEST_F(EngineFixture, WriteCommitsAndReadSeesIt) {
  ASSERT_TRUE(
      engine.Write(0, {"k"}, [](WriteTxn& t, int64_t) { return t.Put(ColumnFamily::kMeta, "k", "v"); }).ok());
  EXPECT_EQ(ReadMeta(engine, "k"), "v");
}

TEST_F(EngineFixture, FailingFnRollsBackAndReturnsItsStatus) {
  const Status st = engine.Write(0, {"k"}, [](WriteTxn& t, int64_t) {
    EXPECT_TRUE(t.Put(ColumnFamily::kMeta, "k", "v").ok());
    return Status::UserError("ERR nope");
  });
  EXPECT_EQ(st.code(), StatusCode::kUserError);
  EXPECT_EQ(st.message(), "ERR nope");
  EXPECT_EQ(ReadMeta(engine, "k"), "<missing>");
}

TEST_F(EngineFixture, SnapshotDoesNotSeeLaterCommits) {
  ASSERT_TRUE(
      engine.Write(0, {"k"}, [](WriteTxn& t, int64_t) { return t.Put(ColumnFamily::kMeta, "k", "1"); }).ok());
  ASSERT_TRUE(engine
                  .Snapshot([&](ReadView& v, int64_t) {
                    auto txn = storage.BeginWrite();  // bypasses the gate on purpose (not re-entrant)
                    EXPECT_TRUE((*txn)->Put(ColumnFamily::kMeta, "k", "2").ok());
                    EXPECT_TRUE((*txn)->Commit().ok());
                    EXPECT_EQ(*v.Get(ColumnFamily::kMeta, "k"), "1");
                    return Status::Ok();
                  })
                  .ok());
  EXPECT_EQ(ReadMeta(engine, "k"), "2");
}

TEST(Engine, ConflictIsRetriedUpToThreeAttempts) {
  ConflictingStorage storage(2);
  LockManager locks;
  FakeClock clock;
  Engine engine(storage, locks, clock);
  int calls = 0;
  const Status st = engine.Write(0, {"k"}, [&](WriteTxn& t, int64_t) {
    ++calls;
    return t.Put(ColumnFamily::kMeta, "k", "v");
  });
  EXPECT_TRUE(st.ok()) << st.ToString();
  EXPECT_EQ(calls, 3);
}

TEST(Engine, EachCommitAttemptReadsTheClockAgain) {
  ConflictingStorage storage(2);
  LockManager locks;
  FakeClock clock(100);
  Engine engine(storage, locks, clock);
  std::vector<int64_t> seen;
  const Status st = engine.Write(0, {"k"}, [&](WriteTxn&, int64_t now_ms) {
    seen.push_back(now_ms);
    clock.Advance(10);
    return Status::Ok();
  });
  EXPECT_TRUE(st.ok()) << st.ToString();
  EXPECT_EQ(seen, (std::vector<int64_t>{100, 110, 120}));
}

#ifndef NDEBUG
TEST(EngineDeathTest, NestedEngineCallAssertsInDebugBuilds) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        InMemoryStorage storage;
        LockManager locks;
        FakeClock clock;
        Engine engine(storage, locks, clock);
        (void)engine.Read(
            [&](ReadView&, int64_t) { return engine.Read([](ReadView&, int64_t) { return Status::Ok(); }); });
      },
      "called back into the Engine");
}
#endif

TEST(Engine, ConflictGivesUpAfterThreeAttempts) {
  ConflictingStorage storage(100);
  LockManager locks;
  FakeClock clock;
  Engine engine(storage, locks, clock);
  int calls = 0;
  const Status st = engine.Write(0, {"k"}, [&](WriteTxn&, int64_t) {
    ++calls;
    return Status::Ok();
  });
  EXPECT_EQ(st.code(), StatusCode::kConflict);
  EXPECT_EQ(calls, Engine::kMaxCommitAttempts);
}

TEST_F(EngineFixture, ConcurrentReadModifyWriteOnOneKeyIsSerialised) {
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 200; ++i) {
        ASSERT_TRUE(engine
                        .Write(0, {"ctr"},
                               [](WriteTxn& txn, int64_t) {
                                 auto cur = txn.Get(ColumnFamily::kMeta, "ctr");
                                 const int n = cur.ok() ? std::stoi(*cur) : 0;
                                 return txn.Put(ColumnFamily::kMeta, "ctr", std::to_string(n + 1));
                               })
                        .ok());
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(ReadMeta(engine, "ctr"), "800");
}

TEST_F(EngineFixture, FnsGetTheClockReadAfterTheyAreCalled) {
  clock.Set(1000);
  int64_t seen = 0;
  ASSERT_TRUE(engine.Read([&](ReadView&, int64_t now_ms) { return seen = now_ms, Status::Ok(); }).ok());
  EXPECT_EQ(seen, 1000);
  clock.Set(2000);
  ASSERT_TRUE(engine.Snapshot([&](ReadView&, int64_t now_ms) { return seen = now_ms, Status::Ok(); }).ok());
  EXPECT_EQ(seen, 2000);
  clock.Set(3000);
  ASSERT_TRUE(
      engine.WriteExclusive([&](WriteTxn&, int64_t now_ms) { return seen = now_ms, Status::Ok(); }).ok());
  EXPECT_EQ(seen, 3000);
}

TEST_F(EngineFixture, WriteReadsTheClockAfterAcquiringTheKeyLocks) {
  std::atomic<bool> in_write{false}, release{false};
  std::thread holder([&] {
    EXPECT_TRUE(engine
                    .Write(0, {"k"},
                           [&](WriteTxn&, int64_t) {
                             in_write = true;
                             while (!release) std::this_thread::yield();
                             return Status::Ok();
                           })
                    .ok());
  });
  while (!in_write) std::this_thread::yield();
  int64_t seen = 0;
  std::thread waiter([&] {
    EXPECT_TRUE(
        engine.Write(0, {"k"}, [&](WriteTxn&, int64_t now_ms) { return seen = now_ms, Status::Ok(); }).ok());
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  clock.Advance(500);  // while the waiter is blocked on the key stripe
  const int64_t after = clock.NowMs();
  release = true;
  holder.join();
  waiter.join();
  EXPECT_EQ(seen, after);
}

TEST_F(EngineFixture, WriteExclusiveWaitsForInflightWrites) {
  std::atomic<bool> in_write{false}, release{false}, exclusive_ran{false};
  std::thread writer([&] {
    EXPECT_TRUE(engine
                    .Write(0, {"k"},
                           [&](WriteTxn&, int64_t) {
                             in_write = true;
                             while (!release) std::this_thread::yield();
                             EXPECT_FALSE(exclusive_ran.load());
                             return Status::Ok();
                           })
                    .ok());
  });
  while (!in_write) std::this_thread::yield();
  std::thread flusher([&] {
    EXPECT_TRUE(engine
                    .WriteExclusive([&](WriteTxn&, int64_t) {
                      exclusive_ran = true;
                      return Status::Ok();
                    })
                    .ok());
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(exclusive_ran.load());
  release = true;
  writer.join();
  flusher.join();
  EXPECT_TRUE(exclusive_ran.load());
}

}  // namespace
}  // namespace tideskv
