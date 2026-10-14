// Regression pin for the "phantom keys after a range delete" bug seen by the Redis differential
// test: keys removed by a committed DeleteRange must not reappear in iterators while a memtable
// flush (Checkpoint) runs concurrently. Reads are tried through every combination of
// {snapshot, read-committed view} x {IterOptions bounds, no IterOptions + manual bound}. Both
// columns use unbounded engine iterators (TidesStorage never opens a bounded one); "bounded" checks
// TidesIterator's own enforcement of IterOptions.
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "storage/tides_storage.h"
#include "support/temp_dir.h"
#include "types/keyspace.h"

namespace tideskv::test {
namespace {

constexpr int kKeys = 20;
const std::string kLo(1, '\x01');
const std::string kHi(1, '\x02');
const std::string kOther(1, '\x03');  // keys outside the queried range

int Iterations() {
  const char* v = std::getenv("TIDESKV_STRESS_ITERS");
  return v != nullptr ? std::atoi(v) : 60;
}

size_t CountKeys(TidesStorage& s, bool snapshot, bool bounded) {
  auto view = snapshot ? s.BeginSnapshot() : s.BeginRead();
  if (!view.ok()) return 0;
  IterOptions opts;
  if (bounded) opts = {kLo, kHi};
  auto it = (*view)->NewIterator(ColumnFamily::kMeta, opts);
  if (!it.ok()) return 0;
  size_t n = 0;
  Status st = bounded ? (*it)->SeekToFirst() : (*it)->Seek(kLo);
  for (; st.ok() && (*it)->Valid(); st = (*it)->Next()) {
    if (!bounded && (*it)->key() >= kHi) break;
    ++n;
  }
  return n;
}

struct Matrix {
  // [snapshot][bounded] -> number of checks that saw phantoms / total checks
  std::array<std::array<std::atomic<int>, 2>, 2> bad{};
  std::array<std::array<std::atomic<int>, 2>, 2> total{};
};

void WriterLoop(TidesStorage& s, std::atomic<int>& deleted, std::atomic<int>& acked, int iters) {
  for (int i = 1; i <= iters; ++i) {
    {
      auto t = s.BeginWrite();
      for (int k = 0; k < kKeys; ++k) {
        ASSERT_TRUE(
            (*t)->Put(ColumnFamily::kMeta, kLo + "k" + std::to_string(i) + "_" + std::to_string(k), "v")
                .ok());
      }
      ASSERT_TRUE((*t)->Commit().ok());
    }
    ASSERT_TRUE(s.Checkpoint().ok());  // the data now lives in an sstable
    {
      auto t = s.BeginWrite();  // a second, newer sstable with overwrites and point deletes
      for (int k = 0; k < kKeys; k += 2) {
        ASSERT_TRUE(
            (*t)->Put(ColumnFamily::kMeta, kLo + "k" + std::to_string(i) + "_" + std::to_string(k), "w")
                .ok());
        ASSERT_TRUE(
            (*t)->Delete(ColumnFamily::kMeta, kLo + "k" + std::to_string(i) + "_" + std::to_string(k + 1))
                .ok());
      }
      ASSERT_TRUE((*t)->Commit().ok());
    }
    ASSERT_TRUE(s.Checkpoint().ok());
    {
      auto t = s.BeginWrite();
      for (ColumnFamily cf : kKeyspaceCfs) {
        ASSERT_TRUE((*t)->DeleteRange(cf, std::string(1, '\0'), std::string(1, '\x10')).ok());
      }
      ASSERT_TRUE((*t)->Commit().ok());
    }
    {
      auto t = s.BeginWrite();  // live keys outside the queried range, written right after the delete, share
                                // the flushed memtable
      for (int k = 0; k < 3; ++k) {
        ASSERT_TRUE(
            (*t)->Put(ColumnFamily::kMeta, kOther + "n" + std::to_string(i) + "_" + std::to_string(k), "v")
                .ok());
      }
      ASSERT_TRUE((*t)->Commit().ok());
    }
    deleted.store(i);
    while (acked.load() < i) std::this_thread::yield();
  }
}

void ReaderLoop(TidesStorage& s, Matrix& m, std::atomic<int>& deleted, std::atomic<int>& acked, int iters) {
  for (int i = 1; i <= iters; ++i) {
    while (deleted.load() < i) std::this_thread::yield();
    const auto stop = std::chrono::steady_clock::now() + std::chrono::milliseconds(15);
    do {
      for (int snap = 0; snap < 2; ++snap) {
        for (int bounded = 0; bounded < 2; ++bounded) {
          m.total[snap][bounded]++;
          if (CountKeys(s, snap != 0, bounded != 0) != 0) m.bad[snap][bounded]++;
        }
      }
    } while (std::chrono::steady_clock::now() < stop);
    acked.store(i);
  }
}

TEST(FlushVisibilityStress, DeletedRangeStaysDeletedWhileCheckpointing) {
  TempDir dir;
  StorageOptions o;
  o.path = (dir.path() / "db").string();
  o.sync_mode = SyncMode::kNone;
  auto s = TidesStorage::Open(o);
  ASSERT_TRUE(s.ok()) << s.status().ToString();
  Matrix m;
  std::atomic<int> deleted{0}, acked{0};
  const int iters = Iterations();
  std::atomic<bool> stop{false};
  std::thread flusher([&] {  // like the compaction scheduler: flushes whenever it likes
    while (!stop.load()) {
      EXPECT_TRUE((*s)->Checkpoint().ok());
      // Checkpoint shares the commit mutex with writers (D1) and std::mutex is not fair: a back-to-back
      // loop would starve the writer. The real scheduler flushes on a timer, not in a spin.
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  std::thread reader([&] { ReaderLoop(**s, m, deleted, acked, iters); });
  WriterLoop(**s, deleted, acked, iters);
  reader.join();
  stop = true;
  flusher.join();
  for (int snap = 0; snap < 2; ++snap) {
    for (int bounded = 0; bounded < 2; ++bounded) {
      EXPECT_EQ(m.bad[snap][bounded].load(), 0)
          << (snap != 0 ? "snapshot " : "read ") << (bounded != 0 ? "bounded" : "unbounded");
      std::printf("[matrix] %-9s %-9s phantoms in %d of %d checks\n", snap != 0 ? "snapshot" : "read",
                  bounded != 0 ? "bounded" : "unbounded", m.bad[snap][bounded].load(),
                  m.total[snap][bounded].load());
    }
  }
}

}  // namespace
}  // namespace tideskv::test
