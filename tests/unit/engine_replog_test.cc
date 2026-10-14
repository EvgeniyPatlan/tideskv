#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/coding.h"
#include "encoding/key_codec.h"
#include "encoding/replog_codec.h"
#include "exec/engine.h"
#include "exec/recording_txn.h"
#include "exec/repl_log.h"
#include "storage/memory_storage.h"
#include "support/conflicting_storage.h"
#include "support/fake_clock.h"
#include "types/hash_type.h"
#include "types/keyspace.h"
#include "types/oid_allocator.h"
#include "types/string_type.h"

namespace tideskv {
namespace {

using encoding::ReplOp;
using encoding::ReplOpType;
using test::ConflictingStorage;
using test::FakeClock;

constexpr int64_t kNow = 1'000'000;

Status SetString(WriteTxn& t, const std::string& key, const std::string& value, int64_t expire_ms,
                 int64_t now) {
  SetOptions opts;
  opts.expire_ms = expire_ms;
  SetResult out;
  return StringSet(t, 0, key, value, opts, now, &out);
}

// The ops a RecordingTxn captures when fn runs alone on a fresh store.
std::vector<ReplOp> CapturedAlone(const Engine::WriteFn& fn) {
  InMemoryStorage scratch;
  auto txn = scratch.BeginWrite();
  RecordingTxn recorder(**txn);
  recorder.CaptureOps(true);
  EXPECT_TRUE(fn(recorder, kNow).ok());
  return recorder.ops();
}

std::vector<ReplOp> BatchAt(Storage& s, uint64_t seq) {
  auto view = s.BeginRead();
  auto raw = (*view)->Get(ColumnFamily::kReplog, encoding::ReplogKey(seq));
  EXPECT_TRUE(raw.ok()) << "no replog entry " << seq;
  if (!raw.ok()) return {};
  auto batch = encoding::DecodeReplogBatch(*raw);
  EXPECT_TRUE(batch.ok());
  return batch.ok() ? batch->ops : std::vector<ReplOp>{};
}

std::vector<ReplOp> Concat(std::vector<ReplOp> a, const std::vector<ReplOp>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

struct EngineReplogTest : ::testing::Test {
  InMemoryStorage storage;
  LockManager locks;
  FakeClock clock{kNow};
  ReplLog log{storage, clock};
  Engine engine{storage, locks, clock, nullptr, nullptr, &log};

  void SetUp() override { ASSERT_TRUE(log.Recover().ok()); }

  static Engine::WriteFn Set(const std::string& key, const std::string& value, int64_t expire_ms = 0) {
    return [=](WriteTxn& t, int64_t now) { return SetString(t, key, value, expire_ms, now); };
  }
  Status Write(const std::string& key, const Engine::WriteFn& fn) { return engine.Write(0, {key}, fn); }
};

TEST_F(EngineReplogTest, EveryCommittedWriteAppendsOneBatch) {
  EXPECT_EQ(engine.repl_log(), &log);
  ASSERT_TRUE(Write("k", Set("k", "v", kNow + 5000)).ok());
  EXPECT_EQ(log.last(), 1u);
  const std::vector<ReplOp> expected = CapturedAlone(Set("k", "v", kNow + 5000));
  ASSERT_EQ(expected.size(), 2u);  // the meta put and the expire-index put
  EXPECT_EQ(expected[0].cf, ColumnFamily::kMeta);
  EXPECT_EQ(expected[1].cf, ColumnFamily::kExpire);
  EXPECT_EQ(BatchAt(storage, 1), expected);
}

TEST_F(EngineReplogTest, FailedWriteAppendsNothing) {
  const Status st = Write("k", [](WriteTxn& t, int64_t now) {
    EXPECT_TRUE(SetString(t, "k", "v", 0, now).ok());
    return Status::UserError("ERR nope");
  });
  EXPECT_EQ(st.code(), StatusCode::kUserError);
  EXPECT_EQ(log.last(), 0u);
}

TEST_F(EngineReplogTest, WriteThatWritesNothingTakesNoSeq) {
  ASSERT_TRUE(Write("k", [](WriteTxn&, int64_t) { return Status::Ok(); }).ok());
  EXPECT_EQ(log.last(), 0u);
}

TEST_F(EngineReplogTest, FencedWriteIsRefusedAndNotApplied) {
  log.FenceLocalWrites(true);
  EXPECT_EQ(Write("k", Set("k", "v")).code(), StatusCode::kUserError);
  auto view = storage.BeginRead();
  EXPECT_TRUE((*view)->Get(ColumnFamily::kMeta, encoding::MetaKeyFor(0, "k")).status().IsNotFound());
}

TEST_F(EngineReplogTest, BatchCommitIsOneReplogBatch) {
  {
    auto batch = engine.BeginBatch();
    ASSERT_TRUE(batch.ok());
    for (const char* k : {"a", "b", "c"}) ASSERT_TRUE(Write(k, Set(k, "v")).ok());
    ASSERT_TRUE((*batch)->Commit().ok());
  }
  EXPECT_EQ(log.last(), 1u);
  EXPECT_EQ(BatchAt(storage, 1), Concat(Concat(CapturedAlone(Set("a", "v")), CapturedAlone(Set("b", "v"))),
                                        CapturedAlone(Set("c", "v"))));
}

TEST_F(EngineReplogTest, BatchSavepointRollbackExcludesTheFailedCommandsOps) {
  {
    auto batch = engine.BeginBatch();
    ASSERT_TRUE(batch.ok());
    ASSERT_TRUE(Write("a", Set("a", "v")).ok());
    EXPECT_FALSE(Write("b", [](WriteTxn& t, int64_t now) {
                   EXPECT_TRUE(SetString(t, "b", "v", 0, now).ok());
                   return Status::UserError("ERR nope");
                 }).ok());
    ASSERT_TRUE(Write("c", Set("c", "v")).ok());
    ASSERT_TRUE((*batch)->Commit().ok());
  }
  EXPECT_EQ(log.last(), 1u);
  EXPECT_EQ(BatchAt(storage, 1), Concat(CapturedAlone(Set("a", "v")), CapturedAlone(Set("c", "v"))));
}

TEST_F(EngineReplogTest, BatchMarkRollbackExcludesTheScriptsOps) {
  {
    auto batch = engine.BeginBatch();
    ASSERT_TRUE(batch.ok());
    auto mark = engine.BatchMark();
    ASSERT_TRUE(mark.ok());
    ASSERT_TRUE(Write("a", Set("a", "v")).ok());
    ASSERT_TRUE(engine.BatchRollbackTo(*mark).ok());
    ASSERT_TRUE(engine.BatchReleaseMark(*mark).ok());
    ASSERT_TRUE((*batch)->Commit().ok());
  }
  EXPECT_EQ(log.last(), 0u);
}

TEST_F(EngineReplogTest, FencedBatchCommitFailsAndAppliesNothing) {
  log.FenceLocalWrites(true);
  {
    auto batch = engine.BeginBatch();
    ASSERT_TRUE(batch.ok());
    ASSERT_TRUE(Write("a", Set("a", "v")).ok());
    EXPECT_EQ((*batch)->Commit().code(), StatusCode::kUserError);
  }
  auto view = storage.BeginRead();
  EXPECT_TRUE((*view)->Get(ColumnFamily::kMeta, encoding::MetaKeyFor(0, "a")).status().IsNotFound());
}

TEST_F(EngineReplogTest, WriteExclusiveFlushIsOneBatchWithRangeDeletes) {
  ASSERT_TRUE(Write("a", Set("a", "v")).ok());
  ASSERT_TRUE(engine.WriteExclusive([](WriteTxn& t, int64_t) { return FlushAll(t); }, {0}).ok());
  EXPECT_EQ(log.last(), 2u);
  const std::vector<ReplOp> ops = BatchAt(storage, 2);
  EXPECT_EQ(ops, CapturedAlone([](WriteTxn& t, int64_t) { return FlushAll(t); }));
  ASSERT_FALSE(ops.empty());
  for (const ReplOp& op : ops) EXPECT_EQ(op.type, ReplOpType::kDeleteRange);
}

TEST_F(EngineReplogTest, OidLeaseIsItsOwnEarlierBatch) {
  OidAllocator oids(storage, {}, [&](const std::vector<ReplOp>& ops) { return log.CommitStandalone(ops); });
  ASSERT_TRUE(oids.Init().ok());
  const auto hset = [&](WriteTxn& t, int64_t now) {
    return HashSet(t, oids, 0, "h", {{"f", "v"}}, now).status();
  };
  ASSERT_TRUE(Write("h", hset).ok());
  EXPECT_EQ(log.last(), 2u);
  const std::vector<ReplOp> lease = BatchAt(storage, 1);
  ASSERT_EQ(lease.size(), 1u);
  EXPECT_EQ(lease[0].type, ReplOpType::kPut);
  EXPECT_EQ(lease[0].cf, ColumnFamily::kSys);
  EXPECT_EQ(lease[0].key, OidAllocator::kHwmKey);
  std::string hwm;
  coding::PutFixed64BE(&hwm, 1 + OidAllocator::kLeaseSize);
  EXPECT_EQ(lease[0].value, hwm);
  const std::vector<ReplOp> data = BatchAt(storage, 2);
  bool meta_put = false;  // the HSET's own ops, not the lease
  for (const ReplOp& op : data) {
    EXPECT_NE(op.cf, ColumnFamily::kSys);
    meta_put |=
        op.type == ReplOpType::kPut && op.cf == ColumnFamily::kMeta && op.key == encoding::MetaKeyFor(0, "h");
  }
  EXPECT_TRUE(meta_put);
}

TEST(EngineReplog, ConflictRetryStillCommitsOnce) {
  ConflictingStorage storage(1);
  LockManager locks;
  FakeClock clock(kNow);
  ReplLog log(storage, clock);
  ASSERT_TRUE(log.Recover().ok());
  Engine engine(storage, locks, clock, nullptr, nullptr, &log);
  int calls = 0;
  ASSERT_TRUE(engine
                  .Write(0, {"k"},
                         [&](WriteTxn& t, int64_t now) {
                           ++calls;
                           return SetString(t, "k", "v", 0, now);
                         })
                  .ok());
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(log.last(), 1u);
  auto view = storage.BeginRead();
  EXPECT_TRUE((*view)->Get(ColumnFamily::kReplog, encoding::ReplogKey(2)).status().IsNotFound());
  EXPECT_EQ(BatchAt(storage, 1),
            CapturedAlone([](WriteTxn& t, int64_t now) { return SetString(t, "k", "v", 0, now); }));
}

TEST(EngineReplog, WithoutAReplLogNothingIsLogged) {
  InMemoryStorage storage;
  LockManager locks;
  FakeClock clock(kNow);
  Engine engine(storage, locks, clock);
  EXPECT_EQ(engine.repl_log(), nullptr);
  ASSERT_TRUE(
      engine.Write(0, {"k"}, [](WriteTxn& t, int64_t now) { return SetString(t, "k", "v", 0, now); }).ok());
  auto view = storage.BeginRead();
  auto it = (*view)->NewIterator(ColumnFamily::kReplog);
  ASSERT_TRUE((*it)->SeekToFirst().ok());
  EXPECT_FALSE((*it)->Valid());
}

}  // namespace
}  // namespace tideskv
