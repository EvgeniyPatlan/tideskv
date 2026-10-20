// A lazy ReplLog with no replica (dormant): commits advance last and cum but write no replog record, and the
// first snapshot that needs the log activates it with a baseline record at S = last.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "common/coding.h"
#include "encoding/replog_codec.h"
#include "exec/repl_log.h"
#include "exec/thread_commit_seq.h"
#include "repl/repl_state.h"
#include "storage/memory_storage.h"
#include "support/fake_clock.h"
#include "types/errors.h"

namespace tideskv {
namespace {

using encoding::ReplOp;
using encoding::ReplOpType;
using test::FakeClock;

constexpr bool kLazy = true;

std::vector<ReplOp> MetaPut(const std::string& key, const std::string& value = "v") {
  return {{ReplOpType::kPut, ColumnFamily::kMeta, key, value}};
}

Status CommitKey(Storage& s, ReplLog& log, const std::string& key) {
  const std::vector<ReplOp> ops = MetaPut(key);
  auto txn = s.BeginWrite();
  if (!txn.ok()) return txn.status();
  if (Status st = (*txn)->Put(ColumnFamily::kMeta, key, "v"); !st.ok()) return st;
  return log.Commit(**txn, ops);
}

std::vector<uint64_t> ReplogSeqs(Storage& s) {
  std::vector<uint64_t> seqs;
  auto view = s.BeginRead();
  auto it = (*view)->NewIterator(ColumnFamily::kReplog);
  EXPECT_TRUE((*it)->SeekToFirst().ok());
  while ((*it)->Valid()) {
    uint64_t seq = 0;
    EXPECT_TRUE(encoding::DecodeReplogKey((*it)->key(), &seq));
    seqs.push_back(seq);
    if (!(*it)->Next().ok()) break;
  }
  return seqs;
}

bool HasSys(Storage& s, std::string_view key) {
  auto view = s.BeginRead();
  return (*view)->Get(ColumnFamily::kSys, key).ok();
}

uint64_t ReplFirstOnDisk(Storage& s) {
  auto view = s.BeginRead();
  auto raw = (*view)->Get(ColumnFamily::kSys, "repl_first");
  if (!raw.ok()) return 0;
  std::string_view in = *raw;
  uint64_t first = 0;
  EXPECT_TRUE(coding::GetFixed64BE(&in, &first));
  return first;
}

repl::PsyncDecision Decide(const ReplLog& log, uint64_t next_seq) {
  const repl::ReplIdentity me{std::string(40, 'a'), {}};
  return repl::DecidePsync(me, {log.first_retained(), log.last(), false}, {me.replid, next_seq});
}

TEST(ReplLogDormantTest, TheDefaultLogIsActive) {
  InMemoryStorage s;
  FakeClock clock;
  ReplLog log(s, clock);
  ASSERT_TRUE(log.Recover().ok());
  EXPECT_TRUE(log.active());
}

TEST(ReplLogDormantTest, CommitsAdvanceLastAndCumWithoutRecords) {
  InMemoryStorage s;
  InMemoryStorage s_active;
  FakeClock clock(1000);
  ReplLog log(s, clock, kLazy);
  ReplLog reference(s_active, clock);
  ASSERT_TRUE(log.Recover().ok());
  ASSERT_TRUE(reference.Recover().ok());
  EXPECT_FALSE(log.active());
  for (int i = 1; i <= 3; ++i) {
    ASSERT_TRUE(CommitKey(s, log, "k" + std::to_string(i)).ok());
    ASSERT_TRUE(CommitKey(s_active, reference, "k" + std::to_string(i)).ok());
    EXPECT_EQ(log.last(), static_cast<uint64_t>(i));
    EXPECT_EQ(engine_internal::t_last_commit_seq, static_cast<uint64_t>(i));  // WAIT still sees the write
  }
  EXPECT_EQ(log.cum_bytes(), reference.cum_bytes());  // the bytes the records would have taken
  EXPECT_TRUE(ReplogSeqs(s).empty());
  EXPECT_EQ(log.first_retained(), 4u);  // nothing before activation is ever offered for CONTINUE
  EXPECT_FALSE(log.active());
  auto view = s.BeginRead();
  EXPECT_EQ(*(*view)->Get(ColumnFamily::kMeta, "k3"), "v");
}

TEST(ReplLogDormantTest, ActivateAndSnapshotWritesTheBaselineAtLast) {
  InMemoryStorage s;
  FakeClock clock(1000);
  ReplLog log(s, clock, kLazy);
  ASSERT_TRUE(log.Recover().ok());
  for (int i = 1; i <= 3; ++i) ASSERT_TRUE(CommitKey(s, log, "k" + std::to_string(i)).ok());
  EXPECT_EQ(Decide(log, 4), repl::PsyncDecision::kContinue);  // the bounds alone; PSYNC activates first
  auto snap = log.ActivateAndSnapshot();
  ASSERT_TRUE(snap.ok()) << snap.status().ToString();
  EXPECT_EQ(snap->seq, 3u);
  EXPECT_TRUE(log.active());
  auto record = snap->view->Get(ColumnFamily::kReplog, encoding::ReplogKey(3));
  ASSERT_TRUE(record.ok());
  auto batch = encoding::DecodeReplogBatch(*record);
  ASSERT_TRUE(batch.ok());
  EXPECT_TRUE(batch->ops.empty());
  EXPECT_EQ(batch->cum_bytes, log.cum_bytes());  // a replica loading it continues the stream byte for byte
  EXPECT_EQ(log.first_retained(), 4u);
  EXPECT_EQ(ReplFirstOnDisk(s), 4u);
  // A replica that holds commit 3 continues; one that would need the unlogged commit 3 does not.
  EXPECT_EQ(Decide(log, 3), repl::PsyncDecision::kFullResync);
  EXPECT_EQ(Decide(log, 4), repl::PsyncDecision::kContinue);
  const uint64_t cum = log.cum_bytes();
  ASSERT_TRUE(CommitKey(s, log, "k4").ok());
  auto raw = s.BeginRead().value()->Get(ColumnFamily::kReplog, encoding::ReplogKey(4));
  ASSERT_TRUE(raw.ok());
  EXPECT_EQ(log.cum_bytes(), cum + raw->size());
  EXPECT_EQ(ReplogSeqs(s), (std::vector<uint64_t>{3, 4}));
  EXPECT_EQ(Decide(log, 5), repl::PsyncDecision::kContinue);
}

TEST(ReplLogDormantTest, ActivationIsStickyAndOnlyWritesOnce) {
  InMemoryStorage s;
  FakeClock clock;
  ReplLog log(s, clock, kLazy);
  ASSERT_TRUE(log.Recover().ok());
  ASSERT_TRUE(CommitKey(s, log, "a").ok());
  ASSERT_TRUE(log.Activate().ok());
  ASSERT_TRUE(CommitKey(s, log, "b").ok());
  ASSERT_TRUE(log.Activate().ok());  // no second baseline over the logged commit 2
  auto snap = log.ActivateAndSnapshot();
  ASSERT_TRUE(snap.ok());
  EXPECT_EQ(snap->seq, 2u);
  EXPECT_EQ(ReplogSeqs(s), (std::vector<uint64_t>{1, 2}));
  auto batch =
      encoding::DecodeReplogBatch(*s.BeginRead().value()->Get(ColumnFamily::kReplog, encoding::ReplogKey(2)));
  ASSERT_TRUE(batch.ok());
  EXPECT_EQ(batch->ops.size(), 1u);
}

TEST(ReplLogDormantTest, AnEmptyLogActivatesWithoutARecord) {
  InMemoryStorage s;
  FakeClock clock;
  ReplLog log(s, clock, kLazy);
  ASSERT_TRUE(log.Recover().ok());
  auto snap = log.ActivateAndSnapshot();
  ASSERT_TRUE(snap.ok());
  EXPECT_EQ(snap->seq, 0u);
  EXPECT_TRUE(log.active());
  EXPECT_TRUE(ReplogSeqs(s).empty());
  EXPECT_EQ(log.first_retained(), 1u);
  EXPECT_EQ(Decide(log, 1), repl::PsyncDecision::kContinue);
}

TEST(ReplLogDormantTest, RecoverTakesTheHead) {
  InMemoryStorage s;
  FakeClock clock;
  uint64_t cum = 0;
  {
    ReplLog log(s, clock, kLazy);
    ASSERT_TRUE(log.Recover().ok());
    for (const char* k : {"a", "b", "c"}) ASSERT_TRUE(CommitKey(s, log, k).ok());
    ASSERT_TRUE(log.PersistHead().ok());  // the trimmer's tick, or a clean shutdown
    cum = log.cum_bytes();
  }
  EXPECT_TRUE(HasSys(s, kSysReplHead));
  EXPECT_TRUE(ReplogSeqs(s).empty());
  ReplLog again(s, clock, kLazy);
  ASSERT_TRUE(again.Recover().ok());
  EXPECT_EQ(again.last(), 3u);
  EXPECT_EQ(again.cum_bytes(), cum);
  EXPECT_EQ(again.first_retained(), 4u);
  EXPECT_FALSE(again.active());
  ASSERT_TRUE(CommitKey(s, again, "d").ok());
  EXPECT_EQ(again.last(), 4u);
}

TEST(ReplLogDormantTest, TheFirstUnloggedCommitPersistsTheHeadWithIt) {
  InMemoryStorage s;
  FakeClock clock;
  {
    ReplLog log(s, clock, kLazy);
    ASSERT_TRUE(log.Recover().ok());
    ASSERT_TRUE(CommitKey(s, log, "a").ok());
    ASSERT_TRUE(CommitKey(s, log, "b").ok());
    // a crash: no PersistHead
  }
  ReplLog again(s, clock, kLazy);
  ASSERT_TRUE(again.Recover().ok());
  EXPECT_EQ(again.last(), 1u);  // commit 2 regressed (by at most a head interval), commit 1 did not
  EXPECT_EQ(again.first_retained(), 2u);
}

TEST(ReplLogDormantTest, ACrashRegressesToTheLastHeadAndTheLogStaysConsistent) {
  InMemoryStorage s;
  FakeClock clock;
  uint64_t head_cum = 0;
  {
    ReplLog log(s, clock, kLazy);
    ASSERT_TRUE(log.Recover().ok());
    for (const char* k : {"a", "b"}) ASSERT_TRUE(CommitKey(s, log, k).ok());
    ASSERT_TRUE(log.PersistHead().ok());  // H = 2
    head_cum = log.cum_bytes();
    for (const char* k : {"c", "d"}) ASSERT_TRUE(CommitKey(s, log, k).ok());
    // a crash: commits 3 and 4 are in the keyspace, the head still says 2
  }
  ReplLog log(s, clock, kLazy);
  ASSERT_TRUE(log.Recover().ok());
  EXPECT_EQ(log.last(), 2u);
  EXPECT_EQ(log.cum_bytes(), head_cum);
  EXPECT_EQ(log.first_retained(), 3u);
  EXPECT_FALSE(log.active());
  ASSERT_TRUE(CommitKey(s, log, "e").ok());
  EXPECT_EQ(log.last(), 3u);  // seq 3 is taken again: it was never seen outside this node
  auto snap = log.ActivateAndSnapshot();
  ASSERT_TRUE(snap.ok());
  EXPECT_EQ(snap->seq, 3u);
  auto batch = encoding::DecodeReplogBatch(*snap->view->Get(ColumnFamily::kReplog, encoding::ReplogKey(3)));
  ASSERT_TRUE(batch.ok());
  EXPECT_EQ(batch->cum_bytes, log.cum_bytes());
  EXPECT_EQ(log.first_retained(), 4u);
  EXPECT_EQ(ReplFirstOnDisk(s), 4u);
  EXPECT_EQ(Decide(log, 3), repl::PsyncDecision::kFullResync);
  EXPECT_EQ(Decide(log, 4), repl::PsyncDecision::kContinue);
}

TEST(ReplLogDormantTest, AStaleHeadIsDeletedAtRecovery) {
  InMemoryStorage s;
  FakeClock clock;
  {
    ReplLog log(s, clock);
    ASSERT_TRUE(log.Recover().ok());
    for (const char* k : {"a", "b"}) ASSERT_TRUE(CommitKey(s, log, k).ok());
  }
  std::string head;
  coding::PutFixed64BE(&head, 1);  // behind the newest record
  coding::PutFixed64BE(&head, 10);
  auto txn = s.BeginWrite();
  ASSERT_TRUE((*txn)->Put(ColumnFamily::kSys, kSysReplHead, head).ok());
  ASSERT_TRUE((*txn)->Commit().ok());
  ReplLog log(s, clock, kLazy);
  ASSERT_TRUE(log.Recover().ok());
  EXPECT_EQ(log.last(), 2u);
  EXPECT_TRUE(log.active());
  EXPECT_FALSE(HasSys(s, kSysReplHead));
}

TEST(ReplLogDormantTest, ADormantLogStillRefusesABatchTooLargeToReplicate) {  // R12
  InMemoryStorage s;
  FakeClock clock;
  ReplLog log(s, clock, kLazy);
  ASSERT_TRUE(log.Recover().ok());
  ASSERT_TRUE(CommitKey(s, log, "a").ok());
  const uint64_t cum = log.cum_bytes();
  log.SetMaxBatchBytes(encoding::ReplogBatchSize(MetaPut("k", std::string(100, 'x'))) - 1);
  const std::vector<ReplOp> big = MetaPut("k", std::string(100, 'x'));
  auto txn = s.BeginWrite();
  ASSERT_TRUE((*txn)->Put(ColumnFamily::kMeta, "k", big[0].value).ok());
  const Status st = log.Commit(**txn, big);
  EXPECT_EQ(st.code(), StatusCode::kUserError);
  EXPECT_EQ(st.message(), kErrTooLargeToReplicate);
  EXPECT_EQ(log.last(), 1u);
  EXPECT_EQ(log.cum_bytes(), cum);
  EXPECT_FALSE(s.BeginRead().value()->Get(ColumnFamily::kMeta, "k").ok());
}

TEST(ReplLogDormantTest, ADemotedDormantLogStillPersistsItsHead) {
  InMemoryStorage s;
  FakeClock clock;
  {
    ReplLog log(s, clock, kLazy);
    ASSERT_TRUE(log.Recover().ok());
    for (const char* k : {"a", "b", "c"}) ASSERT_TRUE(CommitKey(s, log, k).ok());
    log.FenceLocalWrites(true);
    ASSERT_TRUE(log.PersistHead().ok());
    EXPECT_FALSE(log.active());
  }
  ReplLog again(s, clock, kLazy);
  ASSERT_TRUE(again.Recover().ok());
  EXPECT_EQ(again.last(), 3u);
}

TEST(ReplLogDormantTest, AHeadAheadOfStaleRecordsWins) {
  InMemoryStorage s;
  FakeClock clock;
  {
    ReplLog log(s, clock);
    ASSERT_TRUE(log.Recover().ok());
    ASSERT_TRUE(CommitKey(s, log, "a").ok());
  }
  std::string head;
  coding::PutFixed64BE(&head, 5);
  coding::PutFixed64BE(&head, 500);
  auto txn = s.BeginWrite();
  ASSERT_TRUE((*txn)->Put(ColumnFamily::kSys, kSysReplHead, head).ok());
  ASSERT_TRUE((*txn)->Commit().ok());
  ReplLog log(s, clock, kLazy);
  ASSERT_TRUE(log.Recover().ok());
  EXPECT_EQ(log.last(), 5u);
  EXPECT_EQ(log.cum_bytes(), 500u);
  EXPECT_EQ(log.first_retained(), 6u);  // the record 1 is never offered
  EXPECT_FALSE(log.active());
  ASSERT_TRUE(log.Activate().ok());
  EXPECT_EQ(ReplogSeqs(s), (std::vector<uint64_t>{5}));  // the stale record went with the activation
  EXPECT_EQ(ReplFirstOnDisk(s), 6u);
}

TEST(ReplLogDormantTest, ALogThatHasRecordsStartsActive) {
  InMemoryStorage s;
  FakeClock clock;
  {
    ReplLog log(s, clock);
    ASSERT_TRUE(log.Recover().ok());
    ASSERT_TRUE(CommitKey(s, log, "a").ok());
    ASSERT_TRUE(CommitKey(s, log, "b").ok());
  }
  // Active at its last shutdown (it had a replica): a replica reconnecting after the restart must still
  // find every commit since, so the log is not made dormant again.
  ReplLog log(s, clock, kLazy);
  ASSERT_TRUE(log.Recover().ok());
  EXPECT_TRUE(log.active());
  EXPECT_EQ(log.first_retained(), 1u);
  ASSERT_TRUE(CommitKey(s, log, "c").ok());
  EXPECT_EQ(ReplogSeqs(s), (std::vector<uint64_t>{1, 2, 3}));
}

TEST(ReplLogDormantTest, PersistHeadWritesNothingWhileEverythingIsLogged) {
  InMemoryStorage s;
  FakeClock clock;
  ReplLog active(s, clock);
  ASSERT_TRUE(active.Recover().ok());
  ASSERT_TRUE(CommitKey(s, active, "a").ok());
  ASSERT_TRUE(active.PersistHead().ok());
  EXPECT_FALSE(HasSys(s, kSysReplHead));
  InMemoryStorage s2;
  ReplLog lazy(s2, clock, kLazy);
  ASSERT_TRUE(lazy.Recover().ok());
  ASSERT_TRUE(lazy.PersistHead().ok());  // an empty dormant log
  EXPECT_FALSE(HasSys(s2, kSysReplHead));
}

TEST(ReplLogDormantTest, AnActivatedLogRecoversFromItsRecordsAndDropsTheHead) {
  InMemoryStorage s;
  FakeClock clock;
  {
    ReplLog log(s, clock, kLazy);
    ASSERT_TRUE(log.Recover().ok());
    for (const char* k : {"a", "b", "c"}) ASSERT_TRUE(CommitKey(s, log, k).ok());
    ASSERT_TRUE(log.PersistHead().ok());
    ASSERT_TRUE(log.Activate().ok());
    EXPECT_FALSE(HasSys(s, kSysReplHead));
    ASSERT_TRUE(CommitKey(s, log, "d").ok());
    ASSERT_TRUE(CommitKey(s, log, "e").ok());
    ASSERT_TRUE(log.PersistHead().ok());  // active: the records carry last and cum
    EXPECT_FALSE(HasSys(s, kSysReplHead));
  }
  ReplLog again(s, clock, kLazy);
  ASSERT_TRUE(again.Recover().ok());
  EXPECT_EQ(again.last(), 5u);
  EXPECT_EQ(again.first_retained(), 4u);
  EXPECT_EQ(ReplogSeqs(s), (std::vector<uint64_t>{3, 4, 5}));
}

TEST(ReplLogDormantTest, AnActiveLogRecoveringAHeadActivatesAtIt) {
  InMemoryStorage s;
  FakeClock clock;
  uint64_t cum = 0;
  {
    ReplLog log(s, clock, kLazy);
    ASSERT_TRUE(log.Recover().ok());
    for (const char* k : {"a", "b"}) ASSERT_TRUE(CommitKey(s, log, k).ok());
    ASSERT_TRUE(log.PersistHead().ok());
    cum = log.cum_bytes();
  }
  ReplLog active(s, clock);  // repl-backlog-lazy no on the next start
  ASSERT_TRUE(active.Recover().ok());
  EXPECT_TRUE(active.active());
  EXPECT_EQ(active.last(), 2u);
  EXPECT_EQ(active.cum_bytes(), cum);
  EXPECT_EQ(active.first_retained(), 3u);
  EXPECT_EQ(ReplogSeqs(s), (std::vector<uint64_t>{2}));
  EXPECT_FALSE(HasSys(s, kSysReplHead));
  ASSERT_TRUE(CommitKey(s, active, "c").ok());
  ReplLog again(s, clock);
  ASSERT_TRUE(again.Recover().ok());
  EXPECT_EQ(again.last(), 3u);
  EXPECT_EQ(again.first_retained(), 3u);
}

TEST(ReplLogDormantTest, AReplicatedBatchActivatesADormantLog) {
  InMemoryStorage s;
  FakeClock clock;
  ReplLog log(s, clock, kLazy);
  ASSERT_TRUE(log.Recover().ok());
  for (const char* k : {"a", "b"}) ASSERT_TRUE(CommitKey(s, log, k).ok());
  ASSERT_TRUE(log.PersistHead().ok());
  log.FenceLocalWrites(true);  // demoted; its master continues its history
  const std::string payload = encoding::EncodeReplogBatch(MetaPut("c"), 5, log.cum_bytes());
  auto txn = s.BeginWrite();
  ASSERT_TRUE((*txn)->Put(ColumnFamily::kMeta, "c", "v").ok());
  ASSERT_TRUE(log.CommitReplicated(**txn, 3, payload).ok());
  EXPECT_TRUE(log.active());
  EXPECT_EQ(log.last(), 3u);
  EXPECT_EQ(ReplogSeqs(s), (std::vector<uint64_t>{2, 3}));
  EXPECT_FALSE(HasSys(s, kSysReplHead));
  ReplLog again(s, clock, kLazy);
  ASSERT_TRUE(again.Recover().ok());
  EXPECT_EQ(again.last(), 3u);
  EXPECT_EQ(again.first_retained(), 3u);
}

TEST(ReplLogDormantTest, AFullSyncLoadActivatesAndClearsTheHead) {
  InMemoryStorage s;
  FakeClock clock;
  ReplLog log(s, clock, kLazy);
  ASSERT_TRUE(log.Recover().ok());
  for (const char* k : {"a", "b"}) ASSERT_TRUE(CommitKey(s, log, k).ok());
  ASSERT_TRUE(log.PersistHead().ok());
  log.FenceLocalWrites(true);
  auto start = s.BeginWrite();
  ASSERT_TRUE(log.CommitLoadStart(**start).ok());
  EXPECT_TRUE(log.active());
  EXPECT_FALSE(HasSys(s, kSysReplHead));
  auto end = s.BeginWrite();
  const std::string record = encoding::EncodeReplogBatch({}, 5, 100);
  ASSERT_TRUE(log.CommitLoadEnd(**end, 7, record).ok());
  ReplLog again(s, clock, kLazy);
  ASSERT_TRUE(again.Recover().ok());
  EXPECT_EQ(again.last(), 7u);
  EXPECT_EQ(again.first_retained(), 7u);
}

TEST(ReplLogDormantTest, TrimmingAfterActivationAlsoDropsTheBaseline) {
  InMemoryStorage s;
  FakeClock clock;
  ReplLog log(s, clock, kLazy);
  ASSERT_TRUE(log.Recover().ok());
  for (const char* k : {"a", "b"}) ASSERT_TRUE(CommitKey(s, log, k).ok());
  ASSERT_TRUE(log.Activate().ok());  // baseline at 2
  for (const char* k : {"c", "d"}) ASSERT_TRUE(CommitKey(s, log, k).ok());
  EXPECT_EQ(log.first_retained(), 3u);
  ASSERT_TRUE(log.Trim(3).ok());
  EXPECT_EQ(ReplogSeqs(s), (std::vector<uint64_t>{4}));
  EXPECT_EQ(log.first_retained(), 4u);
}

TEST(ReplLogDormantTest, AHeadMakesTheBaselineKnown) {
  InMemoryStorage s;
  FakeClock clock;
  {
    ReplLog log(s, clock, kLazy);
    ASSERT_TRUE(log.Recover().ok());
    ASSERT_TRUE(CommitKey(s, log, "a").ok());
    ASSERT_TRUE(log.PersistHead().ok());
  }
  EXPECT_TRUE(ReplogSeqs(s).empty());
  ReplLog again(s, clock, kLazy);
  ASSERT_TRUE(again.Recover().ok());
  auto unknown = repl::LoadOrDetectBaselineUnknown(s, again.last());
  ASSERT_TRUE(unknown.ok());
  EXPECT_FALSE(*unknown);  // keys and no records, but the head says the log is only dormant
}

TEST(ReplLogDormantTest, AMalformedHeadIsCorruption) {
  InMemoryStorage s;
  FakeClock clock;
  auto txn = s.BeginWrite();
  ASSERT_TRUE((*txn)->Put(ColumnFamily::kSys, kSysReplHead, "short").ok());
  ASSERT_TRUE((*txn)->Commit().ok());
  ReplLog log(s, clock, kLazy);
  EXPECT_EQ(log.Recover().code(), StatusCode::kCorruption);
}

TEST(ReplogCodecSize, BatchSizeMatchesTheEncoding) {
  const std::vector<std::vector<ReplOp>> cases = {
      {},
      MetaPut("k"),
      {{ReplOpType::kPut, ColumnFamily::kSubkey, std::string(300, 'x'), std::string(70000, 'y')},
       {ReplOpType::kDelete, ColumnFamily::kMeta, "gone", ""},
       {ReplOpType::kDeleteRange, ColumnFamily::kZScore, "a", "b"}},
  };
  for (const auto& ops : cases) {
    EXPECT_EQ(encoding::ReplogBatchSize(ops), encoding::EncodeReplogBatch(ops, 1, 0).size());
  }
}

}  // namespace
}  // namespace tideskv
