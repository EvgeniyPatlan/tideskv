#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "encoding/key_codec.h"
#include "encoding/meta_codec.h"
#include "exec/engine.h"
#include "exec/lock_manager.h"
#include "server/special_key_service.h"
#include "storage/tides_storage.h"
#include "support/fake_clock.h"
#include "support/temp_dir.h"
#include "types/special_key_filter.h"

// B1 on the real engine: the startup scan (fresh read views per chunk) against TidesDB's commit visibility
// while writers add collections and TTL keys through the Engine.
namespace tideskv {
namespace {

std::string Meta(encoding::ValueType type, int64_t expire_ms) {
  encoding::Metadata m;
  m.type = type;
  m.expire_ms = expire_ms;
  if (type != encoding::ValueType::kString) m.oid = 1;
  return encoding::EncodeMetadata(m, type == encoding::ValueType::kString ? "v" : "");
}
std::string Hash() { return Meta(encoding::ValueType::kHash, 0); }
std::string Ttl() { return Meta(encoding::ValueType::kString, 4'000'000'000'000); }

TEST(SpecialKeyServiceTides, ScanRacesWithWriters) {
  test::TempDir dir;
  StorageOptions o;
  o.path = (dir.path() / "db").string();
  o.sync_mode = SyncMode::kNone;
  auto storage = TidesStorage::Open(o);
  ASSERT_TRUE(storage.ok()) << storage.status().ToString();
  {
    auto txn = (*storage)->BeginWrite();  // before any hook: only the scan can find these
    ASSERT_TRUE(txn.ok());
    for (int i = 0; i < 20000; ++i) {
      ASSERT_TRUE((*txn)
                      ->Put(ColumnFamily::kMeta, encoding::MetaKeyFor(0, "seed" + std::to_string(i)),
                            i % 2 ? Hash() : Ttl())
                      .ok());
    }
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  SpecialKeyFilter filter;
  (*storage)->SetMetaPutHook(&filter);
  LockManager locks;
  test::FakeClock clock;
  Engine engine(**storage, locks, clock);
  SpecialKeyService service(filter, **storage, engine);
  std::vector<std::thread> writers;
  for (int t = 0; t < 4; ++t) {
    writers.emplace_back([&, t] {
      for (int i = 0; i < 2000; ++i) {
        const std::string key = "w" + std::to_string(t) + "_" + std::to_string(i);
        const std::string mk = encoding::MetaKeyFor(0, key);
        const std::string value = i % 2 ? Hash() : Ttl();
        EXPECT_TRUE(
            engine
                .Write(0, {key}, [&](WriteTxn& w, int64_t) { return w.Put(ColumnFamily::kMeta, mk, value); })
                .ok());
      }
    });
  }
  service.Start();
  for (auto& w : writers) w.join();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (filter.state() != SpecialKeyFilter::State::kReady && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(filter.state(), SpecialKeyFilter::State::kReady);
  auto view = (*storage)->BeginRead();
  ASSERT_TRUE(view.ok());
  auto c = CheckSpecialKeys(**view, filter);
  ASSERT_TRUE(c.ok()) << c.status().ToString();
  EXPECT_EQ(c->specials, 28000u);
  EXPECT_EQ(c->missing, 0u);
  view->reset();
  service.Stop();
  (*storage)->SetMetaPutHook(nullptr);
}

// The startup scan reads the meta CF without filling the block cache: run on the service's long-lived thread,
// cached blocks would be freed cross-thread by the evicting workers and never reused (about one extra cache
// of resident memory), and the pass would evict the workload's hot set.
TEST(SpecialKeyServiceTides, ScanLeavesBlockCacheUntouched) {
  test::TempDir dir;
  StorageOptions o;
  o.path = (dir.path() / "db").string();
  o.sync_mode = SyncMode::kNone;
  auto storage = TidesStorage::Open(o);
  ASSERT_TRUE(storage.ok()) << storage.status().ToString();
  {
    auto txn = (*storage)->BeginWrite();
    ASSERT_TRUE(txn.ok());
    for (int i = 0; i < 20000; ++i) {
      ASSERT_TRUE((*txn)
                      ->Put(ColumnFamily::kMeta, encoding::MetaKeyFor(0, "k" + std::to_string(i)),
                            i % 3 ? Hash() : Ttl())
                      .ok());
    }
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  ASSERT_TRUE((*storage)->Checkpoint().ok());  // the scan reads sstables, not the memtable
  auto before = (*storage)->Stats();
  ASSERT_TRUE(before.ok());
  SpecialKeyFilter filter;
  std::atomic<bool> stop{false};
  ASSERT_TRUE(ScanSpecialKeys(**storage, filter, stop).ok());
  auto after = (*storage)->Stats();
  ASSERT_TRUE(after.ok());
  EXPECT_EQ(after->block_cache_entries, before->block_cache_entries);
  EXPECT_EQ(after->block_cache_bytes, before->block_cache_bytes);
  auto view = (*storage)->BeginRead();
  ASSERT_TRUE(view.ok());
  auto c = CheckSpecialKeys(**view, filter);
  ASSERT_TRUE(c.ok()) << c.status().ToString();
  EXPECT_EQ(c->specials, 20000u);
  EXPECT_EQ(c->missing, 0u);
}

}  // namespace
}  // namespace tideskv
