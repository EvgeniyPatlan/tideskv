#include "server/special_key_service.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "encoding/key_codec.h"
#include "encoding/meta_codec.h"
#include "encoding/replog_codec.h"
#include "exec/engine.h"
#include "exec/lock_manager.h"
#include "storage/memory_storage.h"
#include "support/fake_clock.h"
#include "support/faulty_storage.h"
#include "support/replica_stack.h"
#include "types/special_key_filter.h"

namespace tideskv {
namespace {

using test::FakeClock;

std::string Meta(encoding::ValueType type, int64_t expire_ms = 0) {
  encoding::Metadata m;
  m.type = type;
  m.expire_ms = expire_ms;
  if (type != encoding::ValueType::kString) m.oid = 1;
  return encoding::EncodeMetadata(m, type == encoding::ValueType::kString ? "v" : "");
}
std::string Plain() { return Meta(encoding::ValueType::kString); }
std::string Hash() { return Meta(encoding::ValueType::kHash); }
std::string Ttl() { return Meta(encoding::ValueType::kString, 4'000'000'000'000); }

// Straight into storage: no Engine, so only a hook (if installed) sees it.
void Seed(Storage& s, const std::vector<std::pair<std::string, std::string>>& kvs) {
  auto txn = s.BeginWrite();
  ASSERT_TRUE(txn.ok());
  for (const auto& [k, v] : kvs)
    ASSERT_TRUE((*txn)->Put(ColumnFamily::kMeta, encoding::MetaKeyFor(0, k), v).ok());
  ASSERT_TRUE((*txn)->Commit().ok());
}

SpecialKeyCheck Check(Storage& s, const SpecialKeyFilter& f) {
  auto view = s.BeginRead();
  EXPECT_TRUE(view.ok());
  if (!view.ok()) return {};
  auto r = CheckSpecialKeys(**view, f);
  EXPECT_TRUE(r.ok()) << r.status().ToString();
  return r.ok() ? *r : SpecialKeyCheck{};
}

Status PutThroughEngine(Engine& e, const std::string& key, const std::string& value) {
  const std::string mk = encoding::MetaKeyFor(0, key);
  return e.Write(0, {key}, [&](WriteTxn& t, int64_t) { return t.Put(ColumnFamily::kMeta, mk, value); });
}

bool WaitFor(const std::function<bool()>& cond, std::chrono::milliseconds limit = std::chrono::seconds(5)) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    if (cond()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return cond();
}

struct SpecialKeyServiceTest : ::testing::Test {
  InMemoryStorage storage;
  LockManager locks;
  FakeClock clock;
  Engine engine{storage, locks, clock};
};

TEST_F(SpecialKeyServiceTest, StartupScanFindsExistingSpecials) {
  std::vector<std::pair<std::string, std::string>> kvs = {{"h", Hash()},
                                                          {"z", Meta(encoding::ValueType::kZSet)},
                                                          {"l", Meta(encoding::ValueType::kList)},
                                                          {"s", Meta(encoding::ValueType::kSet)},
                                                          {"t", Ttl()}};
  for (int i = 0; i < 100; ++i) kvs.emplace_back("plain" + std::to_string(i), Plain());
  Seed(storage, kvs);  // before any hook
  SpecialKeyFilter filter;
  storage.SetMetaPutHook(&filter);
  SpecialKeyService service(filter, storage, engine);
  ASSERT_TRUE(service.RebuildNow(1024).ok());
  const SpecialKeyCheck c = Check(storage, filter);
  EXPECT_EQ(c.specials, 5u);
  EXPECT_EQ(c.missing, 0u);
  EXPECT_EQ(filter.state(), SpecialKeyFilter::State::kReady);
  EXPECT_EQ(filter.Capacity(), 1024u);
  EXPECT_EQ(service.rebuilds(), 1u);
  storage.SetMetaPutHook(nullptr);
}

TEST_F(SpecialKeyServiceTest, ScanCrossesChunkBoundaries) {
  std::vector<std::pair<std::string, std::string>> kvs;
  for (size_t i = 0; i < kScanChunk + 10; ++i) kvs.emplace_back("k" + std::to_string(i), Hash());
  Seed(storage, kvs);
  SpecialKeyFilter filter;
  const std::atomic<bool> stop{false};
  ASSERT_TRUE(ScanSpecialKeys(storage, filter, stop).ok());
  EXPECT_EQ(Check(storage, filter).missing, 0u);
  EXPECT_EQ(filter.Count(), kScanChunk + 10);
}

TEST_F(SpecialKeyServiceTest, ScanStopsEarlyWhenAsked) {
  Seed(storage, {{"h", Hash()}});
  SpecialKeyFilter filter;
  const std::atomic<bool> stop{true};
  EXPECT_TRUE(ScanSpecialKeys(storage, filter, stop).ok());
  EXPECT_EQ(filter.Count(), 0u);
}

// The startup scan while 4 writers add specials through the Engine. InMemoryStorage copies every table on
// each commit, so this runs at a smaller size than the TidesStorage version (contract tests: 20k + 4 x 2000).
TEST_F(SpecialKeyServiceTest, ScanRacesWithWriters) {
  constexpr int kSeeded = 4000;
  constexpr int kPerWriter = 500;
  std::vector<std::pair<std::string, std::string>> kvs;
  for (int i = 0; i < kSeeded; ++i) kvs.emplace_back("seed" + std::to_string(i), i % 2 ? Hash() : Ttl());
  Seed(storage, kvs);
  SpecialKeyFilter filter;
  storage.SetMetaPutHook(&filter);
  SpecialKeyService service(filter, storage, engine);
  std::vector<std::thread> writers;
  for (int t = 0; t < 4; ++t) {
    writers.emplace_back([&, t] {
      for (int i = 0; i < kPerWriter; ++i) {
        const std::string key = "w" + std::to_string(t) + "_" + std::to_string(i);
        EXPECT_TRUE(PutThroughEngine(engine, key, i % 2 ? Hash() : Ttl()).ok());
      }
    });
  }
  service.Start();
  for (auto& w : writers) w.join();
  ASSERT_TRUE(WaitFor([&] { return filter.state() == SpecialKeyFilter::State::kReady; }));
  const SpecialKeyCheck c = Check(storage, filter);
  EXPECT_EQ(c.specials, static_cast<uint64_t>(kSeeded + 4 * kPerWriter));
  EXPECT_EQ(c.missing, 0u);
  service.Stop();
  storage.SetMetaPutHook(nullptr);
}

// The ordering Quiesce exists for: a writer that told the hook (old bits) before Reset and commits after it
// must still end up in the rebuilt filter.
TEST_F(SpecialKeyServiceTest, RebuildWaitsForAnOpenWriter) {
  SpecialKeyFilter filter;
  storage.SetMetaPutHook(&filter);
  SpecialKeyService service(filter, storage, engine);
  std::atomic<bool> in_write{false}, release{false};
  std::thread writer([&] {
    const std::string mk = encoding::MetaKeyFor(0, "late");
    EXPECT_TRUE(engine
                    .Write(0, {"late"},
                           [&](WriteTxn& t, int64_t) {
                             if (Status st = t.Put(ColumnFamily::kMeta, mk, Hash()); !st.ok()) return st;
                             in_write = true;
                             while (!release) std::this_thread::yield();
                             return Status::Ok();
                           })
                    .ok());
  });
  while (!in_write) std::this_thread::yield();
  std::atomic<bool> rebuilt{false};
  std::thread rebuilder([&] {
    EXPECT_TRUE(service.RebuildNow(1024).ok());
    rebuilt = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(rebuilt.load());
  release = true;
  writer.join();
  rebuilder.join();
  EXPECT_EQ(filter.state(), SpecialKeyFilter::State::kReady);
  EXPECT_EQ(Check(storage, filter).missing, 0u);
  EXPECT_TRUE(filter.MayContain(encoding::MetaKeyFor(0, "late")));
  storage.SetMetaPutHook(nullptr);
}

TEST_F(SpecialKeyServiceTest, GrowthRebuildRaisesCapacity) {
  SpecialKeyFilter filter(1024);
  storage.SetMetaPutHook(&filter);
  SpecialKeyService service(filter, storage, engine);
  service.Start();
  ASSERT_TRUE(WaitFor([&] { return service.rebuilds() >= 1; }));
  for (int i = 0; i < 5000; ++i) ASSERT_TRUE(PutThroughEngine(engine, "h" + std::to_string(i), Hash()).ok());
  ASSERT_TRUE(WaitFor([&] {
    return filter.Capacity() >= 4096 && filter.state() == SpecialKeyFilter::State::kReady &&
           !filter.NeedsGrowth();
  }));
  EXPECT_GE(service.rebuilds(), 2u);
  const SpecialKeyCheck c = Check(storage, filter);
  EXPECT_EQ(c.specials, 5000u);
  EXPECT_EQ(c.missing, 0u);
  service.Stop();
  storage.SetMetaPutHook(nullptr);
}

TEST(SpecialKeyServiceLoad, RebuildAfterLoadEnds) {
  InMemoryStorage storage;
  test::ReplicaStack replica(storage);
  SpecialKeyFilter filter;
  storage.SetMetaPutHook(&filter);
  SpecialKeyService service(filter, storage, replica.engine);
  service.Start();
  ASSERT_TRUE(WaitFor([&] { return service.rebuilds() == 1; }));
  ASSERT_TRUE(replica.engine.BeginLoad({}).ok());
  ASSERT_TRUE(replica.engine
                  .LoadChunk({{encoding::ReplOpType::kPut, ColumnFamily::kMeta, encoding::MetaKeyFor(0, "h"),
                               Hash()}})
                  .ok());
  std::this_thread::sleep_for(std::chrono::milliseconds(300));  // the watch loop polls every 100 ms
  EXPECT_EQ(service.rebuilds(), 1u);                            // nothing rebuilds while loading
  ASSERT_TRUE(replica.engine.EndLoad(0, "", {}).ok());
  ASSERT_TRUE(WaitFor([&] { return service.rebuilds() == 2; }));
  ASSERT_TRUE(WaitFor([&] { return filter.state() == SpecialKeyFilter::State::kReady; }));
  const SpecialKeyCheck c = Check(storage, filter);
  EXPECT_EQ(c.specials, 1u);
  EXPECT_EQ(c.missing, 0u);
  service.Stop();
  storage.SetMetaPutHook(nullptr);
}

TEST(SpecialKeyServiceErrors, ScanErrorKeepsBuilding) {
  test::FaultyStorage storage;
  LockManager locks;
  FakeClock clock;
  Engine engine{storage, locks, clock};
  SpecialKeyFilter filter;
  storage.SetMetaPutHook(&filter);
  SpecialKeyService service(filter, storage, engine);
  storage.fail_begin_read_in = 1;
  EXPECT_FALSE(service.RebuildNow(1024).ok());
  EXPECT_EQ(filter.state(), SpecialKeyFilter::State::kBuilding);
  EXPECT_EQ(service.rebuilds(), 0u);
  storage.SetMetaPutHook(nullptr);
}

TEST(SpecialKeyServiceErrors, FailedStartupRebuildIsRetried) {
  test::FaultyStorage storage;
  LockManager locks;
  FakeClock clock;
  Engine engine{storage, locks, clock};
  SpecialKeyFilter filter;
  storage.SetMetaPutHook(&filter);
  SpecialKeyService service(filter, storage, engine);
  storage.fail_begin_read_in = 1;
  service.Start();
  ASSERT_TRUE(WaitFor([&] { return filter.state() == SpecialKeyFilter::State::kReady; }));
  EXPECT_EQ(service.rebuilds(), 1u);
  service.Stop();
  storage.SetMetaPutHook(nullptr);
}

TEST_F(SpecialKeyServiceTest, SaturatedFilterIsMarked) {
  std::vector<std::pair<std::string, std::string>> kvs;
  for (int i = 0; i < 3000; ++i) kvs.emplace_back("h" + std::to_string(i), Hash());
  Seed(storage, kvs);
  SpecialKeyFilter filter(1024, /*max_bytes=*/1280);  // MaxCapacity() == 1024
  SpecialKeyService service(filter, storage, engine);
  ASSERT_TRUE(service.RebuildNow(1 << 20).ok());
  EXPECT_EQ(filter.Capacity(), 1024u);
  EXPECT_EQ(filter.state(), SpecialKeyFilter::State::kSaturated);
  EXPECT_FALSE(filter.NeedsGrowth());
}

TEST_F(SpecialKeyServiceTest, StopIsIdempotentAndStartedServiceStopsOnDestruction) {
  SpecialKeyFilter filter;
  storage.SetMetaPutHook(&filter);
  {
    SpecialKeyService service(filter, storage, engine);
    service.Start();
    service.Stop();
    service.Stop();
  }
  {
    SpecialKeyService service(filter, storage, engine);
    service.Start();
  }  // the destructor stops and joins
  SpecialKeyService never_started(filter, storage, engine);
  never_started.Stop();
  storage.SetMetaPutHook(nullptr);
}

}  // namespace
}  // namespace tideskv
