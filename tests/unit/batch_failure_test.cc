#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "encoding/key_codec.h"
#include "exec/engine.h"
#include "support/command_harness.h"
#include "support/fake_clock.h"
#include "support/faulty_storage.h"

namespace tideskv::test {
namespace {

struct FaultyBatch : ::testing::Test {
  FaultyStorage storage;
  LockManager locks;
  FakeClock clock{1000};
  Engine engine{storage, locks, clock};

  Status Put(const std::string& key, const std::string& v) {
    return engine.Write(0, {key}, [&](WriteTxn& t, int64_t) {
      return t.Put(ColumnFamily::kMeta, encoding::MetaKeyFor(0, key), v);
    });
  }
  bool Exists(const std::string& key) {
    bool found = false;
    EXPECT_TRUE(engine
                    .Read([&](ReadView& v, int64_t) {
                      found = v.Get(ColumnFamily::kMeta, encoding::MetaKeyFor(0, key)).ok();
                      return Status::Ok();
                    })
                    .ok());
    return found;
  }
};

TEST_F(FaultyBatch, FailedCommitAppliesNothingAndFreesTheGate) {
  auto batch = engine.BeginBatch();
  ASSERT_TRUE(batch.ok());
  ASSERT_TRUE(Put("a", "1").ok());
  storage.fail_commit_in = 1;
  EXPECT_EQ((*batch)->Commit().code(), StatusCode::kIOError);
  EXPECT_FALSE(engine.InBatch());
  batch->reset();
  EXPECT_FALSE(Exists("a"));
  EXPECT_TRUE(Put("b", "2").ok());  // the gate was released
  EXPECT_TRUE(Exists("b"));
}

TEST_F(FaultyBatch, FailedSavepointRollbackPoisonsTheBatch) {
  auto batch = engine.BeginBatch();
  ASSERT_TRUE(batch.ok());
  ASSERT_TRUE(Put("a", "1").ok());
  storage.fail_rollback_to_savepoint_in = 1;
  EXPECT_FALSE(engine
                   .Write(0, {"b"},
                          [](WriteTxn& t, int64_t) {
                            EXPECT_TRUE(
                                t.Put(ColumnFamily::kMeta, encoding::MetaKeyFor(0, "b"), "half").ok());
                            return Status::UserError("ERR x");
                          })
                   .ok());
  const Status later = Put("c", "3");
  EXPECT_EQ(later.code(), StatusCode::kIOError);  // broken_: refused
  EXPECT_EQ((*batch)->Commit().code(), StatusCode::kIOError);
  batch->reset();
  EXPECT_FALSE(Exists("a"));
  EXPECT_FALSE(Exists("b"));
  EXPECT_FALSE(Exists("c"));
}

TEST_F(FaultyBatch, FailedSavepointFailsOnlyThatWrite) {
  auto batch = engine.BeginBatch();
  ASSERT_TRUE(batch.ok());
  storage.fail_savepoint_in = 1;
  EXPECT_EQ(Put("a", "1").code(), StatusCode::kIOError);
  ASSERT_TRUE(Put("b", "2").ok());
  ASSERT_TRUE((*batch)->Commit().ok());
  batch->reset();
  EXPECT_FALSE(Exists("a"));
  EXPECT_TRUE(Exists("b"));
}

// Task 13: a batch releases each write's savepoint, failed or not, and the marks it is told to release,
// so TidesDB's linear savepoint lookup stays short however long the batch is.
TEST_F(FaultyBatch, WritesAndReleasedMarksLeaveNoSavepointBehind) {
  auto batch = engine.BeginBatch();
  ASSERT_TRUE(batch.ok());
  for (int i = 0; i < 100; ++i) {
    const std::string k = "k" + std::to_string(i);
    const Status st = engine.Write(0, {k}, [&](WriteTxn& t, int64_t) {
      EXPECT_TRUE(t.Put(ColumnFamily::kMeta, encoding::MetaKeyFor(0, k), "v").ok());
      return i % 3 == 0 ? Status::UserError("ERR x") : Status::Ok();
    });
    EXPECT_EQ(st.ok(), i % 3 != 0);
  }
  EXPECT_THROW(
      (void)engine.Write(0, {"t"}, [](WriteTxn&, int64_t) -> Status { throw std::runtime_error("x"); }),
      std::runtime_error);
  EXPECT_EQ(storage.unreleased_savepoints.load(), 0);
  auto mark = engine.BatchMark();
  ASSERT_TRUE(mark.ok());
  ASSERT_TRUE(Put("m", "1").ok());
  EXPECT_EQ(storage.unreleased_savepoints.load(), 1);  // only the mark
  ASSERT_TRUE(engine.BatchReleaseMark(*mark).ok());
  EXPECT_EQ(storage.unreleased_savepoints.load(), 0);
  ASSERT_TRUE((*batch)->Commit().ok());
  batch->reset();
  EXPECT_FALSE(Exists("k0"));
  EXPECT_TRUE(Exists("k1"));
  EXPECT_TRUE(Exists("m"));  // a released mark keeps its writes
}

TEST_F(FaultyBatch, FailedSavepointReleaseDoesNotPoisonTheBatch) {
  auto batch = engine.BeginBatch();
  ASSERT_TRUE(batch.ok());
  storage.fail_release_savepoint_in = 1;
  ASSERT_TRUE(Put("a", "1").ok());  // its savepoint stays: slower, not wrong
  storage.fail_release_savepoint_in = 1;
  EXPECT_FALSE(engine.Write(0, {"b"}, [](WriteTxn&, int64_t) { return Status::UserError("ERR x"); }).ok());
  ASSERT_TRUE(Put("c", "3").ok());
  ASSERT_TRUE((*batch)->Commit().ok());
  batch->reset();
  EXPECT_TRUE(Exists("a"));
  EXPECT_FALSE(Exists("b"));
  EXPECT_TRUE(Exists("c"));
}

// Task 13: every script releases its batch mark, alone or inside EXEC.
TEST(FaultyExec, ScriptsLeaveNoSavepointBehind) {
  CommandHarness h;
  EXPECT_EQ(h.Run({"EVAL", "return redis.call('incr','a')", "0"}), ":1\r\n");
  h.Run({"MULTI"});
  for (int i = 0; i < 3; ++i) h.Run({"EVAL", "redis.call('incr','a') return redis.call('incr','b')", "0"});
  h.Run({"EVAL", "return redis.call('incr')", "0"});  // a script error: the mark is released too
  h.Run({"EXEC"});
  EXPECT_EQ(h.Run({"GET", "a"}), "$1\r\n4\r\n");
  EXPECT_EQ(h.services.storage_.unreleased_savepoints.load(), 0);
}

// M5a ruling R4: a failed EXEC commit applies nothing, connection state included.
TEST(FaultyExec, FailedCommitRestoresTheConnectionAndAppliesNothing) {
  CommandHarness h;
  h.Run({"MULTI"});
  h.Run({"SELECT", "2"});
  h.Run({"SET", "k", "v"});
  h.Run({"HELLO", "3"});
  h.services.storage_.fail_commit_in = 1;
  const std::string r = h.Run({"EXEC"});
  EXPECT_EQ(r.substr(0, 5), "-ERR ") << r;
  EXPECT_FALSE(h.session.multi.active);
  EXPECT_EQ(h.session.db, 0);
  EXPECT_EQ(h.session.protocol, 2);
  EXPECT_EQ(h.reply.protocol(), 2);
  h.Run({"SELECT", "2"});
  EXPECT_EQ(h.Run({"GET", "k"}), kNil);
}

}  // namespace
}  // namespace tideskv::test
