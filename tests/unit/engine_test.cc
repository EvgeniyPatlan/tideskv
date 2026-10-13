#include "exec/engine.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "encoding/key_codec.h"
#include "exec/commit_listener.h"
#include "exec/write_observer.h"
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
  Status ReleaseSavepoint(std::string_view n) override { return inner_->ReleaseSavepoint(n); }
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

class RecordingListener final : public CommitListener {
 public:
  void OnCommitted(const std::vector<RangeDelete>& d) override { batches.push_back(d); }
  std::vector<std::vector<RangeDelete>> batches;
};

TEST(EngineListener, ReportsCommittedRangeDeletesOnly) {
  InMemoryStorage storage;
  LockManager locks;
  FakeClock clock;
  RecordingListener listener;
  Engine e(storage, locks, clock, &listener);
  ASSERT_TRUE(
      e.Write(0, {"k"}, [](WriteTxn& t, int64_t) { return t.Put(ColumnFamily::kMeta, "x", "y"); }).ok());
  EXPECT_TRUE(listener.batches.empty());  // no range deletes: no call
  ASSERT_TRUE(e.Write(0, {"k"}, [](WriteTxn& t, int64_t) {
                 return t.DeletePrefix(ColumnFamily::kSubkey, "p");
               }).ok());
  ASSERT_EQ(listener.batches.size(), 1u);
  EXPECT_EQ(listener.batches[0][0].lo, "p");
  const Status failed = e.Write(0, {"k"}, [](WriteTxn& t, int64_t) {
    (void)t.DeletePrefix(ColumnFamily::kSubkey, "q");
    return Status::UserError("ERR nope");
  });
  EXPECT_FALSE(failed.ok());
  EXPECT_EQ(listener.batches.size(), 1u);  // rolled back: not reported
  ASSERT_TRUE(e.WriteExclusive([](WriteTxn& t, int64_t) {
                 return t.DeletePrefix(ColumnFamily::kMeta, "\x01");
               }).ok());
  EXPECT_EQ(listener.batches.size(), 2u);
}

// Records what the Engine reports; `watched` makes it non-idle.
class RecordingObserver final : public WriteObserver {
 public:
  bool Idle() const override { return !watching; }
  void OnModified(const std::vector<DbKey>& keys, const CommittedViewFn& committed) override {
    reports.push_back(keys);
    had_view = committed() != nullptr;
  }
  std::vector<std::string> ExistingWatched(uint8_t db, ReadView& view) override {
    std::vector<std::string> out;
    for (const auto& k : flush_candidates) {
      if (view.Get(ColumnFamily::kMeta, encoding::MetaKeyFor(db, k)).ok()) out.push_back(k);
    }
    return out;
  }
  bool watching = true;
  bool had_view = false;
  std::vector<std::string> flush_candidates;
  std::vector<std::vector<DbKey>> reports;
};

struct ObservedEngine : ::testing::Test {
  InMemoryStorage storage;
  LockManager locks;
  FakeClock clock;
  RecordingObserver observer;
  Engine engine{storage, locks, clock, nullptr, &observer};

  Status PutMeta(uint8_t db, const std::string& key) {
    return engine.Write(db, {key}, [&](WriteTxn& t, int64_t) {
      return t.Put(ColumnFamily::kMeta, encoding::MetaKeyFor(db, key), "v");
    });
  }
};

TEST_F(ObservedEngine, OneKeyWriteReportsItsKeyAfterCommit) {
  ASSERT_TRUE(engine
                  .Write(2, {"k"},
                         [](WriteTxn& t, int64_t) {
                           return t.Put(ColumnFamily::kSubkey, "x", "v");  // no meta write: rule (b)
                         })
                  .ok());
  ASSERT_EQ(observer.reports.size(), 1u);
  EXPECT_EQ(observer.reports[0], (std::vector<DbKey>{{2, "k"}}));
  EXPECT_TRUE(observer.had_view);
}

TEST_F(ObservedEngine, WriteThatWritesNothingReportsNothing) {
  ASSERT_TRUE(engine.Write(0, {"k"}, [](WriteTxn&, int64_t) { return Status::Ok(); }).ok());
  EXPECT_TRUE(observer.reports.empty());
}

TEST_F(ObservedEngine, FailedWriteReportsNothing) {
  EXPECT_FALSE(engine
                   .Write(0, {"k"},
                          [](WriteTxn& t, int64_t) {
                            EXPECT_TRUE(t.Put(ColumnFamily::kMeta, encoding::MetaKeyFor(0, "k"), "v").ok());
                            return Status::UserError("ERR nope");
                          })
                   .ok());
  EXPECT_TRUE(observer.reports.empty());
}

TEST_F(ObservedEngine, MultiKeyWriteReportsOnlyKeysWhoseMetaChanged) {
  ASSERT_TRUE(engine
                  .Write(0, {"src", "dst"},
                         [](WriteTxn& t, int64_t) {
                           return t.Put(ColumnFamily::kMeta, encoding::MetaKeyFor(0, "dst"), "v");
                         })
                  .ok());
  ASSERT_EQ(observer.reports.size(), 1u);
  EXPECT_EQ(observer.reports[0], (std::vector<DbKey>{{0, "dst"}}));
}

TEST_F(ObservedEngine, AlsoModifiedIsReportedEvenWithoutWrites) {
  std::vector<std::string> also;
  ASSERT_TRUE(engine
                  .Write(
                      0, {"a", "b"},
                      [&](WriteTxn&, int64_t) {
                        also.push_back("b");
                        return Status::Ok();
                      },
                      &also)
                  .ok());
  ASSERT_EQ(observer.reports.size(), 1u);
  EXPECT_EQ(observer.reports[0], (std::vector<DbKey>{{0, "b"}}));
}

TEST_F(ObservedEngine, AlsoModifiedIsClearedBeforeEachAttempt) {
  std::vector<std::string> also = {"stale"};
  ASSERT_TRUE(engine.Write(0, {"a"}, [&](WriteTxn&, int64_t) { return Status::Ok(); }, &also).ok());
  EXPECT_TRUE(also.empty());
  EXPECT_TRUE(observer.reports.empty());
}

TEST_F(ObservedEngine, DuplicatesAreReportedOnce) {
  ASSERT_TRUE(engine
                  .Write(0, {"k"},
                         [](WriteTxn& t, int64_t) {
                           EXPECT_TRUE(t.Put(ColumnFamily::kMeta, encoding::MetaKeyFor(0, "k"), "1").ok());
                           return t.Put(ColumnFamily::kMeta, encoding::MetaKeyFor(0, "k"), "2");
                         })
                  .ok());
  EXPECT_EQ(observer.reports.at(0), (std::vector<DbKey>{{0, "k"}}));
}

TEST_F(ObservedEngine, IdleObserverIsNotCalled) {
  observer.watching = false;
  ASSERT_TRUE(PutMeta(0, "k").ok());
  EXPECT_TRUE(observer.reports.empty());
}

TEST_F(ObservedEngine, FlushReportsWatchedKeysThatExistedBeforeIt) {
  ASSERT_TRUE(PutMeta(4, "here").ok());
  observer.reports.clear();
  observer.flush_candidates = {"here", "absent"};
  ASSERT_TRUE(
      engine
          .WriteExclusive(
              [](WriteTxn& t, int64_t) { return t.DeletePrefix(ColumnFamily::kMeta, encoding::DbPrefix(4)); },
              {4})
          .ok());
  ASSERT_EQ(observer.reports.size(), 1u);
  EXPECT_EQ(observer.reports[0], (std::vector<DbKey>{{4, "here"}}));
}

// Review Focus #1: the report happens before the gate is released, so an exclusive gate
// holder (EXEC) can never run between a commit and its report.
TEST(Engine, ReportHappensBeforeTheGateIsReleased) {
  InMemoryStorage storage;
  LockManager locks;
  FakeClock clock;
  struct SlowObserver final : WriteObserver {
    bool Idle() const override { return false; }
    void OnModified(const std::vector<DbKey>&, const CommittedViewFn&) override {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      reported = true;
    }
    std::vector<std::string> ExistingWatched(uint8_t, ReadView&) override { return {}; }
    std::atomic<bool> reported{false};
  } observer;
  Engine engine(storage, locks, clock, nullptr, &observer);
  std::thread writer([&] {
    EXPECT_TRUE(engine
                    .Write(0, {"k"},
                           [](WriteTxn& t, int64_t) {
                             return t.Put(ColumnFamily::kMeta, encoding::MetaKeyFor(0, "k"), "v");
                           })
                    .ok());
  });
  // Spin until the commit is visible, then take the gate exclusively: the report must be done.
  while (ReadMeta(engine, encoding::MetaKeyFor(0, "k")) == "<missing>") std::this_thread::yield();
  ASSERT_TRUE(engine
                  .WriteExclusive([&](WriteTxn&, int64_t) {
                    EXPECT_TRUE(observer.reported.load());
                    return Status::Ok();
                  })
                  .ok());
  writer.join();
}

}  // namespace
}  // namespace tideskv
