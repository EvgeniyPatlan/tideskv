#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "encoding/replog_codec.h"
#include "repl/repl_state.h"
#include "storage/memory_storage.h"
#include "support/command_harness.h"
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
  auto snap = master.services.repl_log_.SnapshotAtLast();
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
  EXPECT_EQ(DigestOf(replica.engine), kEmptyDigest);
  EXPECT_FALSE(replica.listener.seen.empty());  // the wipe's range deletes go to compaction
}

TEST_F(EngineLoadTest, StartupDiscardsAnInterruptedLoad) {
  {
    ReplicaStack crashed(replica_storage);  // a process that dies mid-load: no AbortLoad, no EndLoad
    ASSERT_TRUE(crashed.engine.BeginLoad({}).ok());
    auto snap = master.services.repl_log_.SnapshotAtLast();
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

}  // namespace
}  // namespace tideskv::test
