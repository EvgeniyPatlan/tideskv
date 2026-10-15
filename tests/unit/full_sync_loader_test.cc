#include "repl/full_sync_loader.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "encoding/replog_codec.h"
#include "repl/full_sync_frames.h"
#include "storage/memory_storage.h"
#include "support/command_harness.h"
#include "support/replica_stack.h"
#include "support/snapshot_frames.h"

namespace tideskv::repl {
namespace {

using namespace std::chrono_literals;
using Args = std::vector<std::string>;

struct LoaderTest : ::testing::Test {
  LoaderTest() {
    for (int i = 0; i < 50; ++i) master.Run({"SADD", "s" + std::to_string(i % 4), "m" + std::to_string(i)});
    own.replid = std::string(40, 'c');
    own.history = {{std::string(40, 'd'), 9}};
  }
  FullSyncLoader::Hooks Hooks() {
    FullSyncLoader::Hooks h;
    h.identity = [this] { return own; };
    h.publish_identity = [this](const ReplIdentity& id) { published.push_back(id); };
    h.on_baseline_known = [this] { ++baseline_known; };
    return h;
  }
  // The master's stream after +FULLRESYNC, as frames.
  std::vector<Args> Stream(uint64_t* seq) {
    test::SnapshotStream s = test::SnapshotFrames(master.services.repl_log_);
    *seq = s.seq;
    return s.frames;
  }

  test::CommandHarness master;
  InMemoryStorage storage;
  test::ReplicaStack replica{storage};
  ReplIdentity own;
  std::vector<ReplIdentity> published;
  int baseline_known = 0;
};

TEST_F(LoaderTest, LoadsTheSnapshotAndAdoptsTheMastersReplidWithNoHistory) {
  uint64_t seq = 0;
  const auto frames = Stream(&seq);
  FullSyncLoader loader(replica.engine, std::string(40, 'a'), seq, Hooks());
  ASSERT_TRUE(loader.Begin().ok());
  EXPECT_TRUE(replica.engine.loading());
  ASSERT_EQ(published.size(), 1u);
  EXPECT_EQ(published[0].replid, own.replid);  // the start keeps our id and drops the history
  EXPECT_TRUE(published[0].history.empty());
  EXPECT_EQ(baseline_known, 1);
  bool done = false;
  ASSERT_TRUE(loader.HandleFrame({"PING"}, &done).ok());
  for (const Args& f : frames) ASSERT_TRUE(loader.HandleFrame(f, &done).ok());
  EXPECT_TRUE(done);
  EXPECT_FALSE(replica.engine.loading());
  EXPECT_EQ(replica.log.last(), seq);
  EXPECT_EQ(test::DigestOf(replica.engine), test::DigestOf(master.services.engine_));
  ASSERT_EQ(published.size(), 2u);
  EXPECT_EQ(published[1].replid, std::string(40, 'a'));
  EXPECT_TRUE(published[1].history.empty());
  EXPECT_GT(loader.loaded_bytes(), 0u);
  auto persisted = LoadOrCreateIdentity(storage);  // the end txn persisted the adopted identity
  ASSERT_TRUE(persisted.ok());
  EXPECT_EQ(persisted->replid, std::string(40, 'a'));
  EXPECT_TRUE(persisted->history.empty());
  EXPECT_EQ(loader.HandleFrame({"PING"}, &done).code(), StatusCode::kIOError);  // nothing after LOADEND
}

TEST_F(LoaderTest, DestroyingAnUnfinishedLoaderAbortsTheLoad) {
  uint64_t seq = 0;
  const auto frames = Stream(&seq);
  {
    FullSyncLoader loader(replica.engine, std::string(40, 'a'), seq, Hooks());
    ASSERT_TRUE(loader.Begin().ok());
    bool done = false;
    ASSERT_TRUE(loader.HandleFrame(frames[0], &done).ok());
  }
  EXPECT_FALSE(replica.engine.loading());
  EXPECT_FALSE(replica.log.load_incomplete());
  EXPECT_EQ(test::DigestOf(replica.engine), std::string(40, '0'));
}

TEST_F(LoaderTest, ProtocolViolationsAreErrors) {
  uint64_t seq = 0;
  auto frames = Stream(&seq);
  ASSERT_GE(frames.size(), 2u);
  FullSyncLoader loader(replica.engine, std::string(40, 'a'), seq, Hooks());
  ASSERT_TRUE(loader.Begin().ok());
  bool done = false;
  // CF-M6b-4: a BATCH is never applied while loading.
  EXPECT_EQ(loader.HandleFrame({"BATCH", "1", "x"}, &done).code(), StatusCode::kIOError);
  EXPECT_EQ(loader.HandleFrame({"LOAD"}, &done).code(), StatusCode::kIOError);
  Args corrupt = frames[0];
  corrupt[1][5] ^= 1;
  EXPECT_EQ(loader.HandleFrame(corrupt, &done).code(), StatusCode::kCorruption);
  for (size_t i = 0; i + 1 < frames.size(); ++i) ASSERT_TRUE(loader.HandleFrame(frames[i], &done).ok());
  Args wrong_seq = frames.back();  // every chunk is loaded: only the seq is wrong (C10)
  wrong_seq[1] = std::to_string(seq + 1);
  EXPECT_EQ(loader.HandleFrame(wrong_seq, &done).code(), StatusCode::kIOError);
  Args wrong_count = frames.back();
  wrong_count[3] = "999999";
  EXPECT_EQ(loader.HandleFrame(wrong_count, &done).code(), StatusCode::kIOError);
  Args malformed = frames.back();
  malformed[3] = "x";
  EXPECT_EQ(loader.HandleFrame(malformed, &done).code(), StatusCode::kIOError);
  EXPECT_FALSE(done);
  EXPECT_TRUE(replica.engine.loading());
  ASSERT_TRUE(loader.HandleFrame(frames.back(), &done).ok());  // the right one still ends the load
  EXPECT_TRUE(done);
}

// CF-M6b-6: a snapshot chunk is a standalone batch; a cum_bytes other than 0 is not one a master sends.
TEST_F(LoaderTest, AChunkWithAStreamOffsetIsCorrupt) {
  FullSyncLoader loader(replica.engine, std::string(40, 'a'), 0, Hooks());
  ASSERT_TRUE(loader.Begin().ok());
  const std::vector<encoding::ReplOp> ops{
      {encoding::ReplOpType::kPut, ColumnFamily::kMeta, std::string("\0\0\0x", 4), "v"}};
  bool done = false;
  EXPECT_EQ(loader.HandleFrame({"LOAD", encoding::EncodeReplogBatch(ops, 1, 7)}, &done).code(),
            StatusCode::kCorruption);
  EXPECT_TRUE(loader.HandleFrame({"LOAD", encoding::EncodeReplogBatch(ops, 1, 0)}, &done).ok());
}

TEST_F(LoaderTest, AnEmptyMasterLoadsAtSeqZero) {
  test::CommandHarness empty;
  test::SnapshotStream s = test::SnapshotFrames(empty.services.repl_log_);
  ASSERT_EQ(s.seq, 0u);
  FullSyncLoader loader(replica.engine, std::string(40, 'a'), 0, Hooks());
  ASSERT_TRUE(loader.Begin().ok());
  bool done = false;
  for (const Args& f : s.frames) ASSERT_TRUE(loader.HandleFrame(f, &done).ok());
  EXPECT_TRUE(done);
  EXPECT_EQ(replica.log.last(), 0u);
  EXPECT_EQ(test::DigestOf(replica.engine), std::string(40, '0'));
}

// key-load-delay (D14) sleeps per loaded key, and a stop request cuts it short (PF14).
TEST_F(LoaderTest, TheKeyLoadDelayIsPerKeyAndStoppable) {
  uint64_t seq = 0;
  const auto frames = Stream(&seq);
  std::atomic<bool> stop{false};
  FullSyncLoader::Hooks h = Hooks();
  h.key_load_delay_us = [] { return 200000; };  // 0.2 s per key: tens of keys would take seconds
  h.stopping = [&] { return stop.load(); };
  FullSyncLoader loader(replica.engine, std::string(40, 'a'), seq, h);
  ASSERT_TRUE(loader.Begin().ok());
  std::thread stopper([&] {
    std::this_thread::sleep_for(300ms);
    stop = true;
  });
  const auto t0 = std::chrono::steady_clock::now();
  bool done = false;
  EXPECT_TRUE(loader.HandleFrame(frames[0], &done).ok());
  const auto took = std::chrono::steady_clock::now() - t0;
  stopper.join();
  EXPECT_GE(took, 250ms);   // it did sleep per key
  EXPECT_LT(took, 1500ms);  // and stopped within one key's delay of the request
}

}  // namespace
}  // namespace tideskv::repl
