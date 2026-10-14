// Backlog trimming over the real engine (M6a D18): after range-deleting 200 000 replog batches, a seek to
// the first retained batch is direct (tidesdb-replication §3 measured 0.02 ms) and a restart recovers the
// bounds from sys/repl_first without a SeekToFirst over the range tombstone.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "encoding/replog_codec.h"
#include "exec/repl_log.h"
#include "repl/backlog_trimmer.h"
#include "storage/tides_storage.h"
#include "support/fake_clock.h"
#include "support/temp_dir.h"

namespace tideskv::test {
namespace {

using namespace std::chrono_literals;
using Ms = std::chrono::duration<double, std::milli>;

constexpr uint64_t kBatches = 200'001;
constexpr uint64_t kPerTxn = 20'000;

StorageOptions Opts(const TempDir& dir) {
  StorageOptions o;
  o.path = (dir.path() / "db").string();
  o.sync_mode = SyncMode::kNone;
  return o;
}

// replog[1..kBatches], each a one-put batch with its cum_bytes chained, written in a few big txns.
void FillReplog(Storage& s) {
  uint64_t cum = 0;
  for (uint64_t seq = 1; seq <= kBatches;) {
    auto txn = s.BeginWrite();
    ASSERT_TRUE(txn.ok());
    for (uint64_t n = 0; n < kPerTxn && seq <= kBatches; ++n, ++seq) {
      const std::vector<encoding::ReplOp> ops = {
          {encoding::ReplOpType::kPut, ColumnFamily::kMeta, "key" + std::to_string(seq), "value"}};
      const std::string record = encoding::EncodeReplogBatch(ops, 1'700'000'000'000, cum);
      cum += record.size();
      ASSERT_TRUE((*txn)->Put(ColumnFamily::kReplog, encoding::ReplogKey(seq), record).ok());
    }
    ASSERT_TRUE((*txn)->Commit().ok());
  }
}

// Seeks the replog to `seq` the way a ReplSender does; returns the seq found there, 0 if none.
uint64_t SeekTo(Storage& s, uint64_t seq, Ms* took) {
  const auto start = std::chrono::steady_clock::now();
  auto view = s.BeginRead();
  EXPECT_TRUE(view.ok());
  auto it = (*view)->NewIterator(ColumnFamily::kReplog, IterOptions{encoding::ReplogKey(seq), {}});
  EXPECT_TRUE(it.ok());
  EXPECT_TRUE((*it)->Seek(encoding::ReplogKey(seq)).ok());
  uint64_t found = 0;
  if ((*it)->Valid()) {
    EXPECT_TRUE(encoding::DecodeReplogKey((*it)->key(), &found));
  }
  *took = std::chrono::steady_clock::now() - start;
  return found;
}

TEST(ReplogTrimTides, SeekToFirstRetainedIsDirectAfterTrimming200000Batches) {
  TempDir dir;
  FakeClock clock;
  {
    auto s = TidesStorage::Open(Opts(dir));
    ASSERT_TRUE(s.ok()) << s.status().ToString();
    FillReplog(**s);
    ReplLog log(**s, clock);
    ASSERT_TRUE(log.Recover().ok());
    ASSERT_EQ(log.last(), kBatches);
    repl::BacklogTrimmer::Hooks hooks;
    hooks.config = [] {
      ConfigValues v;
      v.repl_backlog_size = 1;
      return v;
    };
    repl::BacklogTrimmer trimmer(**s, log, /*compactor=*/nullptr, clock, hooks, {});
    const auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(trimmer.RunOnReplica().ok());  // two range-delete txns of 100 000 batches
    const Ms trim_took = std::chrono::steady_clock::now() - start;
    ASSERT_EQ(log.first_retained(), kBatches);
    EXPECT_EQ(trimmer.trimmed_total(), kBatches - 1);
    Ms seek_took{};
    EXPECT_EQ(SeekTo(**s, kBatches, &seek_took), kBatches);
    EXPECT_LT(seek_took.count(), 50.0);
    RecordProperty("trim_ms", std::to_string(trim_took.count()));
    RecordProperty("seek_first_ms", std::to_string(seek_took.count()));
    std::printf("trim of %llu batches: %.2f ms; seek to the first retained: %.3f ms\n",
                static_cast<unsigned long long>(kBatches - 1), trim_took.count(), seek_took.count());
  }
  auto s = TidesStorage::Open(Opts(dir));  // a restart: the bounds come from sys/repl_first and SeekToLast
  ASSERT_TRUE(s.ok()) << s.status().ToString();
  ReplLog log(**s, clock);
  const auto start = std::chrono::steady_clock::now();
  ASSERT_TRUE(log.Recover().ok());
  const Ms recover_took = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(log.first_retained(), kBatches);
  EXPECT_EQ(log.last(), kBatches);
  EXPECT_LT(recover_took.count(), 50.0);
  Ms seek_took{};
  EXPECT_EQ(SeekTo(**s, kBatches, &seek_took), kBatches);
  EXPECT_LT(seek_took.count(), 50.0);
  ASSERT_TRUE((*s)->Checkpoint().ok());  // the cadence's compaction drops the trimmed keys
  Status compacted = (*s)->Compact(ColumnFamily::kReplog);
  for (int i = 0; i < 100 && compacted.code() == StatusCode::kBusy; ++i) {  // an engine compaction holds it
    std::this_thread::sleep_for(50ms);
    compacted = (*s)->Compact(ColumnFamily::kReplog);
  }
  ASSERT_TRUE(compacted.ok()) << compacted.ToString();
  EXPECT_EQ(SeekTo(**s, kBatches, &seek_took), kBatches);
}

}  // namespace
}  // namespace tideskv::test
