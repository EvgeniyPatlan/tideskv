#include "repl/full_sync_frames.h"

#include <gtest/gtest.h>

#include "cluster/migration_record.h"
#include "common/coding.h"
#include "encoding/replog_codec.h"
#include "storage/memory_storage.h"
#include "support/command_harness.h"
#include "support/key_layout_skip.h"
#include "types/oid_allocator.h"
#include "types/slot_keys.h"

namespace tideskv::repl {
namespace {

using encoding::ReplOp;
using encoding::ReplOpType;

void Put(Storage& s, ColumnFamily cf, const std::string& k, const std::string& v) {
  auto txn = s.BeginWrite();
  ASSERT_TRUE((*txn)->Put(cf, k, v).ok());
  ASSERT_TRUE((*txn)->Commit().ok());
}

std::vector<std::string> AllChunks(ReadView& view, uint64_t* ops) {
  SnapshotChunker c(view, 7);
  std::vector<std::string> out;
  for (;;) {
    auto chunk = c.Next(nullptr);
    EXPECT_TRUE(chunk.ok());
    if (!chunk.ok() || !chunk->has_value()) break;
    out.push_back(**chunk);
  }
  *ops = c.ops();
  return out;
}

TEST(FullSyncFrames, FramesAreRespArrays) {
  EXPECT_EQ(LoadFrame("ab"), "*2\r\n$4\r\nLOAD\r\n$2\r\nab\r\n");
  EXPECT_EQ(LoadEndFrame(12, "rec", 3), "*4\r\n$7\r\nLOADEND\r\n$2\r\n12\r\n$3\r\nrec\r\n$1\r\n3\r\n");
  EXPECT_EQ(LoadEndFrame(0, "", 0), "*4\r\n$7\r\nLOADEND\r\n$1\r\n0\r\n$0\r\n\r\n$1\r\n0\r\n");
}

TEST(FullSyncFrames, OnlyKeyspacePutsAndTheOidHwmAreLoadable) {
  EXPECT_TRUE(IsLoadableOp({ReplOpType::kPut, ColumnFamily::kMeta, "k", "v"}));
  EXPECT_TRUE(IsLoadableOp({ReplOpType::kPut, ColumnFamily::kExpire, "k", "v"}));
  EXPECT_TRUE(IsLoadableOp({ReplOpType::kPut, ColumnFamily::kSys, "oid_hwm", "v"}));
  EXPECT_FALSE(IsLoadableOp({ReplOpType::kPut, ColumnFamily::kSys, "repl_id", "v"}));
  EXPECT_FALSE(IsLoadableOp({ReplOpType::kDelete, ColumnFamily::kMeta, "k", ""}));
  EXPECT_FALSE(IsLoadableOp({ReplOpType::kDeleteRange, ColumnFamily::kSubkey, "a", "b"}));
}

TEST(FullSyncFrames, DecodeLoadChunkRejectsCorruptionAndForeignOps) {
  const std::string good =
      encoding::EncodeReplogBatch({{ReplOpType::kPut, ColumnFamily::kMeta, "k", "v"}}, 1, 0);
  ASSERT_TRUE(DecodeLoadChunk(good).ok());
  std::string flipped = good;
  flipped[3] ^= 1;
  EXPECT_EQ(DecodeLoadChunk(flipped).status().code(), StatusCode::kCorruption);
  const std::string foreign =
      encoding::EncodeReplogBatch({{ReplOpType::kPut, ColumnFamily::kSys, "repl_first", "x"}}, 1, 0);
  EXPECT_EQ(DecodeLoadChunk(foreign).status().code(), StatusCode::kCorruption);
}

TEST(FullSyncFrames, ChunkerCoversEveryLoadableKeyAndNothingElse) {
  InMemoryStorage s;
  Put(s, ColumnFamily::kMeta, "m1", "a");
  Put(s, ColumnFamily::kSubkey, "s1", "b");
  Put(s, ColumnFamily::kZScore, "z1", "");
  Put(s, ColumnFamily::kExpire, "e1", "c");
  Put(s, ColumnFamily::kSys, "oid_hwm", std::string(8, '\x01'));
  Put(s, ColumnFamily::kSys, "repl_id", std::string(40, 'a'));
  Put(s, ColumnFamily::kReplog, encoding::ReplogKey(1), "x");
  auto view = s.BeginSnapshot();
  uint64_t ops = 0;
  const auto chunks = AllChunks(**view, &ops);
  ASSERT_EQ(chunks.size(), 1u);
  auto decoded = DecodeLoadChunk(chunks[0]);
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(*decoded,
            (std::vector<ReplOp>{{ReplOpType::kPut, ColumnFamily::kMeta, "m1", "a"},
                                 {ReplOpType::kPut, ColumnFamily::kSubkey, "s1", "b"},
                                 {ReplOpType::kPut, ColumnFamily::kZScore, "z1", ""},
                                 {ReplOpType::kPut, ColumnFamily::kExpire, "e1", "c"},
                                 {ReplOpType::kPut, ColumnFamily::kSys, "oid_hwm", std::string(8, '\x01')}}));
  EXPECT_EQ(ops, 5u);
}

TEST(FullSyncFrames, ChunksAreBoundedByOpsAndBytes) {  // Review Focus #3, R12
  InMemoryStorage s;
  {
    auto txn = s.BeginWrite();
    for (int i = 0; i < 70000; ++i)
      ASSERT_TRUE((*txn)->Put(ColumnFamily::kMeta, "k" + std::to_string(i), "v").ok());
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  Put(s, ColumnFamily::kSubkey, "big", std::string(5 << 20, 'x'));  // larger than kLoadChunkBytes
  Put(s, ColumnFamily::kExpire, "after", "y");
  auto view = s.BeginSnapshot();
  uint64_t ops = 0;
  const auto chunks = AllChunks(**view, &ops);
  ASSERT_EQ(chunks.size(), 4u);  // 65536 small, 4464 small, the 5 MiB value alone, then "after"
  EXPECT_EQ(DecodeLoadChunk(chunks[0])->size(), kLoadChunkOps);
  EXPECT_EQ(DecodeLoadChunk(chunks[1])->size(), 70000 - kLoadChunkOps);
  EXPECT_EQ(DecodeLoadChunk(chunks[2])->size(), 1u);
  EXPECT_EQ(DecodeLoadChunk(chunks[3])->size(), 1u);
  for (const auto& c : chunks) EXPECT_LE(c.size(), encoding::kMaxReplBatchBytes);
  EXPECT_LE(chunks[1].size(), kLoadChunkBytes + 64);
  EXPECT_EQ(ops, 70002u);
}

TEST(FullSyncFrames, BetweenKeysRunsPerKeyAndItsErrorEndsTheWalk) {
  InMemoryStorage s;
  for (int i = 0; i < 5; ++i) Put(s, ColumnFamily::kMeta, "k" + std::to_string(i), "v");
  auto view = s.BeginSnapshot();
  SnapshotChunker c(**view, 1);
  int calls = 0;
  auto chunk = c.Next([&] { return ++calls == 3 ? Status::IOError("stopped") : Status::Ok(); });
  EXPECT_EQ(chunk.status().code(), StatusCode::kIOError);
  EXPECT_EQ(calls, 3);
}

// M7b D2: the migrator's walk — meta, subkey, zscore of one slot range, no expire CF, no sys record.
TEST(SnapshotChunker, AWalkBoundedToASlotRangeSkipsOtherSlotsTheExpireCfAndSys) {
  TIDESKV_SKIP_IF_SLOTLESS();
  test::CommandHarness h;
  ASSERT_EQ(h.Run({"SET", "b", "1", "PX", "100000"}), test::kOk);  // slot 3300, with a TTL
  ASSERT_EQ(h.Run({"HSET", "{b}h", "f", "v"}), test::Int(1));
  ASSERT_EQ(h.Run({"ZADD", "{b}z", "1", "m"}), test::Int(1));
  ASSERT_EQ(h.Run({"SET", "foo", "1"}), test::kOk);  // slot 12182
  ASSERT_EQ(h.Run({"HSET", "{foo}h", "f", "v"}), test::Int(1));
  auto snap = h.services.repl_log_.ActivateAndSnapshot();
  ASSERT_TRUE(snap.ok());
  ChunkerOptions o;
  o.cfs = {ColumnFamily::kMeta, ColumnFamily::kSubkey, ColumnFamily::kZScore};
  o.bounds = SlotRangeBounds(3300, 3300);
  o.sys_keys = {};
  SnapshotChunker chunker(*snap->view, 0, o);
  std::vector<encoding::ReplOp> ops;
  for (;;) {
    auto next = chunker.Next(nullptr);
    ASSERT_TRUE(next.ok());
    if (!next->has_value()) break;
    auto batch = encoding::DecodeReplogBatch(**next);
    ASSERT_TRUE(batch.ok());
    ops.insert(ops.end(), batch->ops.begin(), batch->ops.end());
  }
  ASSERT_EQ(ops.size(), 6u);  // 3 metas, the hash field and the zset member (subkey), its score (zscore)
  for (size_t i = 0; i < ops.size(); ++i) {
    EXPECT_EQ(ops[i].type, encoding::ReplOpType::kPut);
    EXPECT_EQ(coding::DecodeFixed16BE(ops[i].key.data() + 1), 3300) << i;
  }
  EXPECT_EQ(ops[0].cf, ColumnFamily::kMeta);
  EXPECT_EQ(ops[2].cf, ColumnFamily::kMeta);
  EXPECT_EQ(ops[3].cf, ColumnFamily::kSubkey);
  EXPECT_EQ(ops[5].cf, ColumnFamily::kZScore);
}

TEST(SnapshotChunker, SetMaxBytesSplitsTheNextChunks) {
  TIDESKV_SKIP_IF_SLOTLESS();
  test::CommandHarness h;
  for (int i = 0; i < 20; ++i)
    ASSERT_EQ(h.Run({"SET", "{b}" + std::to_string(i), std::string(100, 'v')}), test::kOk);
  auto snap = h.services.repl_log_.ActivateAndSnapshot();
  ASSERT_TRUE(snap.ok());
  ChunkerOptions o;
  o.cfs = {ColumnFamily::kMeta};
  o.bounds = SlotRangeBounds(3300, 3300);
  o.sys_keys = {};
  SnapshotChunker chunker(*snap->view, 0, o);
  chunker.SetMaxBytes(300);  // about two 130-byte records per chunk
  size_t chunks = 0;
  for (;;) {
    auto next = chunker.Next(nullptr);
    ASSERT_TRUE(next.ok());
    if (!next->has_value()) break;
    ++chunks;
  }
  EXPECT_EQ(chunks, 10u);
  EXPECT_EQ(chunker.ops(), 20u);
}

TEST(SlotKeys, SlotRangeBoundsCoverTheLastSlot) {
  TIDESKV_SKIP_IF_SLOTLESS();
  const IterOptions b = SlotRangeBounds(16380, 16383);
  EXPECT_EQ(b.lower, encoding::MetaSlotPrefix(0, 16380));
  EXPECT_EQ(b.upper, std::string("\x00\x40\x00", 3));
}

TEST(SnapshotChunker, TheMigrationRecordIsStreamedAfterTheOidLeaseAndLoadable) {  // M7b D8
  test::CommandHarness h;
  ASSERT_EQ(h.Run({"HSET", "h", "f", "v"}), test::Int(1));  // leases an oid block
  cluster::MigrationRecord r;
  r.imported = {{1, 2}};
  ASSERT_TRUE(h.services.repl_log_.CommitStandalone({cluster::MigrationRecordPut(r)}).ok());
  auto snap = h.services.repl_log_.ActivateAndSnapshot();
  ASSERT_TRUE(snap.ok());
  SnapshotChunker chunker(*snap->view, 0);
  std::vector<encoding::ReplOp> ops;
  for (;;) {
    auto next = chunker.Next(nullptr);
    ASSERT_TRUE(next.ok());
    if (!next->has_value()) break;
    auto decoded = DecodeLoadChunk(**next);  // every op passes IsLoadableOp
    ASSERT_TRUE(decoded.ok()) << decoded.status().ToString();
    ops.insert(ops.end(), decoded->begin(), decoded->end());
  }
  ASSERT_GE(ops.size(), 2u);
  EXPECT_EQ(ops[ops.size() - 2].key, OidAllocator::kHwmKey);
  EXPECT_EQ(ops.back().key, cluster::kSysMigration);
  EXPECT_EQ(ops.back().value, cluster::EncodeMigrationRecord(r));
  EXPECT_FALSE(IsLoadableOp({encoding::ReplOpType::kPut, ColumnFamily::kSys, "cluster_cfg", "x"}));
}

}  // namespace
}  // namespace tideskv::repl
