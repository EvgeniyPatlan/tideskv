#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "encoding/key_layout.h"
#include "encoding/replog_codec.h"
#include "repl/repl_state.h"
#include "storage/memory_storage.h"
#include "support/command_harness.h"
#include "support/faulty_storage.h"
#include "support/replica_stack.h"
#include "types/errors.h"
#include "types/oid_allocator.h"

namespace tideskv::test {
namespace {

using namespace std::chrono_literals;
using encoding::ReplOp;
using encoding::ReplOpType;

const std::string kEmptyDigest(40, '0');

// Every loadable entry of `view` as a PUT (M6b D3): meta, subkey, zscore, expire, then sys/oid_hwm.
std::vector<ReplOp> SnapshotPuts(ReadView& view) {
  std::vector<ReplOp> ops;
  for (ColumnFamily cf :
       {ColumnFamily::kMeta, ColumnFamily::kSubkey, ColumnFamily::kZScore, ColumnFamily::kExpire}) {
    auto it = view.NewIterator(cf);
    for (Status st = (*it)->SeekToFirst(); st.ok() && (*it)->Valid(); st = (*it)->Next()) {
      ops.push_back({ReplOpType::kPut, cf, std::string((*it)->key()), std::string((*it)->value())});
    }
  }
  if (auto hwm = view.Get(ColumnFamily::kSys, OidAllocator::kHwmKey); hwm.ok()) {
    ops.push_back({ReplOpType::kPut, ColumnFamily::kSys, std::string(OidAllocator::kHwmKey), *hwm});
  }
  return ops;
}

struct EngineLoadTest : ::testing::Test {
  EngineLoadTest() {
    for (int i = 0; i < 20; ++i)
      master.Run({"HSET", "h" + std::to_string(i % 3), "f" + std::to_string(i), "v"});
    master.Run({"SET", "s", "1", "EX", "1000"});
    master.Run({"ZADD", "z", "1.5", "m"});
    master.Run({"SELECT", "15"});
    master.Run({"RPUSH", "l", "a", "b"});
  }
  uint64_t MasterLast() { return master.services.repl_log_.last(); }
  std::string MasterDigest() { return DigestOf(master.services.engine_); }

  CommandHarness master;
  InMemoryStorage replica_storage;
  ReplicaStack replica{replica_storage};
};

const auto kNoRead = [](ReadView&, int64_t) { return Status::Ok(); };
const auto kNoWrite = [](WriteTxn&, int64_t) { return Status::Ok(); };

TEST_F(EngineLoadTest, BeginLoadWipesTheKeyspaceAndTheLogAndRaisesLoading) {
  ASSERT_TRUE(ShipAll(master.services.storage_, MasterLast(), replica.engine).ok());
  ASSERT_NE(DigestOf(replica.engine), kEmptyDigest);
  ASSERT_TRUE(replica.engine.BeginLoad({}).ok());
  EXPECT_TRUE(replica.engine.loading());
  EXPECT_TRUE(replica.log.load_incomplete());
  EXPECT_EQ(replica.log.last(), 0u);
  {
    const Engine::LoadingReadScope debug;
    EXPECT_EQ(DigestOf(replica.engine), kEmptyDigest);
  }
}

TEST_F(EngineLoadTest, CommandsAreRefusedWhileLoading) {
  ASSERT_TRUE(replica.engine.BeginLoad({}).ok());
  for (const Status& st :
       {replica.engine.Read(kNoRead), replica.engine.Snapshot(kNoRead),
        replica.engine.Write(0, {"k"}, kNoWrite), replica.engine.WriteExclusive(kNoWrite)}) {
    EXPECT_EQ(st.code(), StatusCode::kUserError);
    EXPECT_EQ(st.message(), kErrLoading);
  }
  const Engine::LoadingReadScope debug;
  EXPECT_TRUE(replica.engine.Snapshot(kNoRead).ok());
  EXPECT_TRUE(replica.engine.Read(kNoRead).ok());
  EXPECT_EQ(replica.engine.Write(0, {"k"}, kNoWrite).message(),
            kErrLoading);  // the scope never allows writes
}

// PF7 (Redis): an EXEC whose queue touches no data runs while loading; only keyspace access is refused,
// and it is checked under the batch's exclusive gate.
TEST_F(EngineLoadTest, ABatchRunsWhileLoadingButItsKeyspaceAccessIsRefused) {
  ASSERT_TRUE(replica.engine.BeginLoad({}).ok());
  {
    auto empty = replica.engine.BeginBatch();
    ASSERT_TRUE(empty.ok()) << empty.status().ToString();
    EXPECT_TRUE((*empty)->Commit().ok());
  }
  auto batch = replica.engine.BeginBatch();
  ASSERT_TRUE(batch.ok()) << batch.status().ToString();
  for (const Status& st :
       {replica.engine.Read(kNoRead), replica.engine.Snapshot(kNoRead),
        replica.engine.Write(0, {"k"}, kNoWrite), replica.engine.WriteExclusive(kNoWrite)}) {
    EXPECT_EQ(st.code(), StatusCode::kUserError);
    EXPECT_EQ(st.message(), kErrLoading);
  }
  {
    const Engine::LoadingReadScope debug;
    EXPECT_TRUE(replica.engine.Read(kNoRead).ok());
    EXPECT_EQ(replica.engine.Write(0, {"k"}, kNoWrite).message(), kErrLoading);
  }
  EXPECT_TRUE((*batch)->Commit().ok());
}

TEST_F(EngineLoadTest, ACommandWaitingForTheGateNeverSeesAPartialLoad) {
  auto batch = replica.engine.BeginBatch();  // holds the gate exclusively
  ASSERT_TRUE(batch.ok());
  std::atomic<bool> saw_loading{false};
  std::atomic<bool> read_done{false};
  std::thread reader([&] {  // queued on the gate before the load starts
    const Status st = replica.engine.Read([&](ReadView&, int64_t) {
      if (replica.engine.loading()) saw_loading = true;
      return Status::Ok();
    });
    EXPECT_TRUE(st.ok() || st.message() == kErrLoading) << st.ToString();
    read_done = true;
  });
  std::this_thread::sleep_for(50ms);
  std::thread loader([&] { EXPECT_TRUE(replica.engine.BeginLoad({}).ok()); });
  std::this_thread::sleep_for(50ms);
  batch->reset();
  loader.join();
  reader.join();
  EXPECT_TRUE(read_done.load());
  EXPECT_FALSE(saw_loading.load());
}

TEST_F(EngineLoadTest, ChunksAndEndInstallTheMastersStateAndStreamingContinues) {
  ASSERT_TRUE(replica.engine.BeginLoad({}).ok());
  auto snap = master.services.repl_log_.ActivateAndSnapshot();
  ASSERT_TRUE(snap.ok());
  const std::vector<ReplOp> puts = SnapshotPuts(*snap->view);
  const size_t half = puts.size() / 2;
  ASSERT_TRUE(replica.engine.LoadChunk({puts.begin(), puts.begin() + half}).ok());
  ASSERT_TRUE(replica.engine.LoadChunk({puts.begin() + half, puts.end()}).ok());
  auto record = snap->view->Get(ColumnFamily::kReplog, encoding::ReplogKey(snap->seq));
  ASSERT_TRUE(record.ok());
  const repl::ReplIdentity id{std::string(40, 'b'), {}};
  ASSERT_TRUE(replica.engine.EndLoad(snap->seq, *record, repl::IdentityOps(id)).ok());
  EXPECT_FALSE(replica.engine.loading());
  EXPECT_FALSE(replica.log.load_incomplete());
  EXPECT_EQ(replica.log.last(), snap->seq);
  EXPECT_EQ(DigestOf(replica.engine), MasterDigest());
  auto loaded = repl::LoadOrCreateIdentity(replica_storage);
  ASSERT_TRUE(loaded.ok());
  EXPECT_EQ(loaded->replid, id.replid);
  EXPECT_TRUE(loaded->history.empty());
  master.Run({"SET", "after", "1"});
  ASSERT_TRUE(ShipAll(master.services.storage_, MasterLast(), replica.engine).ok());
  EXPECT_EQ(DigestOf(replica.engine), MasterDigest());
}

TEST_F(EngineLoadTest, LoadingRequiresTheFenceAndChunksRequireLoading) {
  EXPECT_EQ(replica.engine.LoadChunk({}).code(), StatusCode::kInternal);
  replica.log.FenceLocalWrites(false);
  EXPECT_EQ(replica.engine.BeginLoad({}).code(), StatusCode::kInternal);
  EXPECT_FALSE(replica.engine.loading());
  EXPECT_FALSE(replica.log.load_incomplete());
}

// CF-M6b-2: EndLoad (and AbortLoad's wipe) only follow a started load; ReplLog::CommitLoadEnd has no
// guard of its own.
TEST_F(EngineLoadTest, LoadStepsRequireAStartedLoad) {
  ASSERT_TRUE(ShipAll(master.services.storage_, MasterLast(), replica.engine).ok());
  const std::string before = DigestOf(replica.engine);
  const uint64_t last = replica.log.last();
  EXPECT_EQ(replica.engine.EndLoad(0, "", {}).code(), StatusCode::kInternal);
  EXPECT_EQ(replica.engine.AbortLoad().code(), StatusCode::kInternal);  // nothing to discard
  replica.log.FenceLocalWrites(false);
  EXPECT_EQ(replica.engine.BeginLoad({}).code(), StatusCode::kInternal);
  EXPECT_EQ(replica.engine.EndLoad(0, "", {}).code(), StatusCode::kInternal);  // after a failed Begin
  replica.log.FenceLocalWrites(true);
  EXPECT_EQ(DigestOf(replica.engine), before);
  EXPECT_EQ(replica.log.last(), last);
  ASSERT_TRUE(replica.engine.BeginLoad({}).ok());
  EXPECT_EQ(replica.engine.BeginLoad({}).code(), StatusCode::kInternal);  // one load at a time
  EXPECT_TRUE(replica.engine.loading());
  ASSERT_TRUE(replica.engine.EndLoad(0, "", {}).ok());
  EXPECT_EQ(replica.engine.EndLoad(0, "", {}).code(), StatusCode::kInternal);
  EXPECT_EQ(replica.engine.AbortLoad().code(), StatusCode::kInternal);
}

TEST_F(EngineLoadTest, BeginEndAndAbortDirtyEveryWatcher) {
  auto watcher = std::make_shared<WatchClient>();
  ASSERT_TRUE(replica.watches.Watch(watcher, 3, "nothing-here", false));
  ASSERT_TRUE(replica.engine.BeginLoad({}).ok());
  EXPECT_TRUE(watcher->dirty());
  replica.watches.UnwatchAll(*watcher);
  ASSERT_TRUE(replica.watches.Watch(watcher, 3, "nothing-here", false));
  ASSERT_TRUE(replica.engine.EndLoad(0, "", {}).ok());
  EXPECT_TRUE(watcher->dirty());
  replica.watches.UnwatchAll(*watcher);
  ASSERT_TRUE(replica.engine.BeginLoad({}).ok());
  ASSERT_TRUE(replica.watches.Watch(watcher, 3, "nothing-here", false));
  ASSERT_TRUE(replica.engine.AbortLoad().ok());
  EXPECT_TRUE(watcher->dirty());
}

TEST_F(EngineLoadTest, AbortLoadDiscardsAPartialLoad) {
  ASSERT_TRUE(replica.engine.BeginLoad({}).ok());
  ASSERT_TRUE(
      replica.engine.LoadChunk({{ReplOpType::kPut, ColumnFamily::kMeta, std::string("\0\0\0k", 4), "x"}})
          .ok());
  ASSERT_TRUE(replica.engine.AbortLoad().ok());
  EXPECT_FALSE(replica.engine.loading());
  EXPECT_FALSE(replica.log.load_incomplete());
  EXPECT_EQ(replica.log.last(), 0u);  // D5: an aborted load leaves an empty replica, log included
  EXPECT_EQ(DigestOf(replica.engine), kEmptyDigest);
  EXPECT_FALSE(replica.listener.seen.empty());  // the wipe's range deletes go to compaction
}

// Task 8 review: the loader's destructor aborts after a failed EndLoad. EndLoad's txn rolled back and
// loading stays set, so AbortLoad still discards; the log is empty (last 0) in memory and after Recover.
TEST_F(EngineLoadTest, AbortLoadAfterAFailedEndLoadEmptiesTheReplica) {
  ASSERT_TRUE(ShipAll(master.services.storage_, MasterLast(), replica.engine).ok());
  ASSERT_GT(replica.log.last(), 0u);
  ASSERT_TRUE(replica.engine.BeginLoad({}).ok());
  ASSERT_TRUE(
      replica.engine.LoadChunk({{ReplOpType::kPut, ColumnFamily::kMeta, std::string("\0\0\0k", 4), "x"}})
          .ok());
  EXPECT_EQ(replica.engine.EndLoad(7, "not a replog record", {}).code(), StatusCode::kCorruption);
  EXPECT_TRUE(replica.engine.loading());
  EXPECT_TRUE(replica.log.load_incomplete());
  ASSERT_TRUE(replica.engine.AbortLoad().ok());
  EXPECT_FALSE(replica.engine.loading());
  EXPECT_FALSE(replica.log.load_incomplete());
  EXPECT_EQ(replica.log.last(), 0u);
  EXPECT_EQ(replica.log.first_retained(), 1u);
  EXPECT_EQ(DigestOf(replica.engine), kEmptyDigest);
  ReplLog again(replica_storage, replica.clock);
  ASSERT_TRUE(again.Recover().ok());
  EXPECT_EQ(again.last(), 0u);
  EXPECT_FALSE(again.load_incomplete());
}

TEST_F(EngineLoadTest, StartupDiscardsAnInterruptedLoad) {
  {
    ReplicaStack crashed(replica_storage);  // a process that dies mid-load: no AbortLoad, no EndLoad
    ASSERT_TRUE(crashed.engine.BeginLoad({}).ok());
    auto snap = master.services.repl_log_.ActivateAndSnapshot();
    ASSERT_TRUE(snap.ok());
    const std::vector<ReplOp> puts = SnapshotPuts(*snap->view);
    ASSERT_TRUE(crashed.engine.LoadChunk({puts.begin(), puts.begin() + 3}).ok());
  }
  ReplicaStack restarted(replica_storage);  // what App::Create does after ReplLog::Recover
  ASSERT_TRUE(restarted.log.load_incomplete());
  EXPECT_FALSE(restarted.engine.loading());
  ASSERT_TRUE(restarted.engine.AbortLoad().ok());
  EXPECT_EQ(DigestOf(restarted.engine), kEmptyDigest);
  ReplLog again(replica_storage, restarted.clock);
  ASSERT_TRUE(again.Recover().ok());
  EXPECT_FALSE(again.load_incomplete());
}

// G1 (final review #1): a batch is never applied onto a partial keyspace, whether the load is running
// (loading) or only its marker survived (a failed abort, or a restart before the startup discard).
TEST_F(EngineLoadTest, ApplyReplicatedIsRefusedWhileLoadingOrMarked) {
  auto first = ReplogEntry(master.services.storage_, 1);
  ASSERT_TRUE(first.ok());
  {
    ReplicaStack crashed(replica_storage);
    ASSERT_TRUE(crashed.engine.BeginLoad({}).ok());
    EXPECT_EQ(crashed.engine.ApplyReplicated(1, *first).code(), StatusCode::kInternal);
    EXPECT_EQ(crashed.log.last(), 0u);
  }
  ReplicaStack restarted(replica_storage);
  ASSERT_TRUE(restarted.log.load_incomplete());
  ASSERT_FALSE(restarted.engine.loading());
  EXPECT_EQ(restarted.engine.ApplyReplicated(1, *first).code(), StatusCode::kInternal);
  EXPECT_EQ(restarted.log.last(), 0u);
  ASSERT_TRUE(restarted.engine.AbortLoad().ok());
  EXPECT_TRUE(restarted.engine.ApplyReplicated(1, *first).ok());
}

// Slotless key layout: a replica adopting its master's layout switches the process inside the load barrier,
// and back when the start txn fails (nothing was wiped, so the data is still in the old layout).
TEST(EngineLoadLayout, BeginLoadAdoptsTheLayoutAndRestoresItOnFailure) {
  const encoding::ScopedKeyLayout slotless(encoding::KeyLayout::kSlotless);
  FaultyStorage storage;
  ReplicaStack replica(storage);
  storage.fail_commit_in = 1;
  EXPECT_EQ(replica.engine.BeginLoad({}, encoding::KeyLayout::kSlotted).code(), StatusCode::kIOError);
  EXPECT_EQ(encoding::CurrentKeyLayout(), encoding::KeyLayout::kSlotless);
  EXPECT_FALSE(replica.engine.loading());
  ASSERT_TRUE(replica.engine.BeginLoad({}, encoding::KeyLayout::kSlotted).ok());
  EXPECT_EQ(encoding::CurrentKeyLayout(), encoding::KeyLayout::kSlotted);
  ASSERT_TRUE(replica.engine.AbortLoad().ok());  // an abort keeps it: the keyspace is empty in any layout
  EXPECT_EQ(encoding::CurrentKeyLayout(), encoding::KeyLayout::kSlotted);
}

// G1: an abort whose commit fails changes nothing (still loading, marker kept, applies refused); the
// retry discards.
TEST(EngineLoadAbort, AFailedAbortKeepsTheLoadAndARetryDiscardsIt) {
  FaultyStorage storage;
  ReplicaStack replica(storage);
  ASSERT_TRUE(replica.engine.BeginLoad({}).ok());
  ASSERT_TRUE(
      replica.engine.LoadChunk({{ReplOpType::kPut, ColumnFamily::kMeta, std::string("\0\0\0k", 4), "x"}})
          .ok());
  storage.fail_commit_in = 1;
  EXPECT_EQ(replica.engine.AbortLoad().code(), StatusCode::kIOError);
  EXPECT_TRUE(replica.engine.loading());
  EXPECT_TRUE(replica.log.load_incomplete());
  ASSERT_TRUE(replica.engine.AbortLoad().ok());
  EXPECT_FALSE(replica.engine.loading());
  EXPECT_FALSE(replica.log.load_incomplete());
  EXPECT_EQ(DigestOf(replica.engine), kEmptyDigest);
}

// G1: the startup discard of a marked node whose log batches had extended (the pre-fix failure chain:
// failed abort, PSYNC ? 1, +CONTINUE, batches 1..N onto the partial keyspace, restart) yields a seq-0
// node: an empty keyspace and an empty log, so its next PSYNC cannot be answered +CONTINUE from N+1.
TEST_F(EngineLoadTest, RestartAfterAnAbortLeavesASeqZeroNode) {
  {
    ReplicaStack crashed(replica_storage);
    ASSERT_TRUE(crashed.engine.BeginLoad({}).ok());
    auto snap = master.services.repl_log_.ActivateAndSnapshot();
    ASSERT_TRUE(snap.ok());
    const std::vector<ReplOp> puts = SnapshotPuts(*snap->view);
    ASSERT_TRUE(crashed.engine.LoadChunk({puts.begin(), puts.begin() + 3}).ok());
    auto txn = replica_storage.BeginWrite();  // what the unguarded applies wrote: replog[1..N]
    ASSERT_TRUE(txn.ok());
    for (uint64_t seq = 1; seq <= MasterLast(); ++seq) {
      auto payload = ReplogEntry(master.services.storage_, seq);
      ASSERT_TRUE(payload.ok());
      ASSERT_TRUE((*txn)->Put(ColumnFamily::kReplog, encoding::ReplogKey(seq), *payload).ok());
    }
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  ReplicaStack restarted(replica_storage);
  ASSERT_TRUE(restarted.log.load_incomplete());
  ASSERT_EQ(restarted.log.last(), MasterLast());
  ASSERT_TRUE(restarted.engine.AbortLoad().ok());  // App::Create's startup discard
  EXPECT_EQ(restarted.log.last(), 0u);
  EXPECT_EQ(restarted.log.first_retained(), 1u);
  EXPECT_EQ(restarted.log.cum_bytes(), 0u);
  EXPECT_EQ(DigestOf(restarted.engine), kEmptyDigest);
  ReplLog again(replica_storage, restarted.clock);
  ASSERT_TRUE(again.Recover().ok());
  EXPECT_FALSE(again.load_incomplete());
  EXPECT_EQ(again.last(), 0u);
  EXPECT_EQ(again.first_retained(), 1u);
  ASSERT_TRUE(ShipAll(master.services.storage_, MasterLast(), restarted.engine).ok());  // streams from 1
  EXPECT_EQ(DigestOf(restarted.engine), MasterDigest());
}

}  // namespace
}  // namespace tideskv::test
