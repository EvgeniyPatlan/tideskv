#include "types/special_key_filter.h"

#include <gtest/gtest.h>

#include <atomic>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "encoding/meta_codec.h"
#include "storage/memory_storage.h"

namespace tideskv {
namespace {

std::string RandomKey(std::mt19937_64& rng) {
  const size_t len = 8 + rng() % 33;
  std::string k(len, '\0');
  for (char& c : k) c = static_cast<char>(rng());
  return k;
}

std::string Plain() { return encoding::EncodeMetadata(encoding::Metadata{}, "v"); }
std::string WithType(encoding::ValueType t, int64_t expire = 0) {
  encoding::Metadata m;
  m.type = t;
  m.expire_ms = expire;
  return encoding::EncodeMetadata(m, "v");
}

TEST(SpecialKeyFilter, NoFalseNegatives) {
  SpecialKeyFilter f;
  std::mt19937_64 rng(1);
  std::vector<std::string> keys;
  for (int i = 0; i < 1000000; ++i) keys.push_back(RandomKey(rng));
  for (const auto& k : keys) f.Insert(k);
  for (const auto& k : keys) ASSERT_TRUE(f.MayContain(k));
}

TEST(SpecialKeyFilter, FalsePositiveRateNearDesign) {
  SpecialKeyFilter f;
  std::mt19937_64 rng(2);
  for (int i = 0; i < 1000000; ++i) f.Insert("in" + RandomKey(rng));
  size_t fp = 0;
  for (int i = 0; i < 1000000; ++i) fp += f.MayContain("out" + RandomKey(rng)) ? 1 : 0;
  const double rate = static_cast<double>(fp) / 1e6;
  std::printf("special-key filter FP rate: %.5f\n", rate);
  EXPECT_LT(rate, 0.02);
}

TEST(SpecialKeyFilter, OnMetaPutInsertsOnlySpecial) {
  SpecialKeyFilter f;
  f.OnMetaPut("plain", Plain());
  EXPECT_EQ(f.Count(), 0u);
  EXPECT_FALSE(f.MayContain("plain"));
  f.OnMetaPut("ttl", WithType(encoding::ValueType::kString, 5));
  f.OnMetaPut("h", WithType(encoding::ValueType::kHash));
  f.OnMetaPut("l", WithType(encoding::ValueType::kList));
  f.OnMetaPut("s", WithType(encoding::ValueType::kSet));
  f.OnMetaPut("z", WithType(encoding::ValueType::kZSet));
  f.OnMetaPut("bad", "x");
  for (const char* k : {"ttl", "h", "l", "s", "z", "bad"}) EXPECT_TRUE(f.MayContain(k)) << k;
  EXPECT_EQ(f.Count(), 6u);
}

TEST(SpecialKeyFilter, CountIsDistinct) {
  SpecialKeyFilter f;
  for (int i = 0; i < 1000; ++i) f.Insert("same");
  EXPECT_EQ(f.Count(), 1u);
}

TEST(SpecialKeyFilter, SkipReadAllowedNeedsReadyAndEnabled) {
  SpecialKeyFilter f;
  EXPECT_FALSE(f.SkipReadAllowed("absent"));
  f.Insert("present");
  f.MarkReady();
  EXPECT_EQ(f.state_name(), "ready");
  EXPECT_TRUE(f.SkipReadAllowed("absent"));
  EXPECT_FALSE(f.SkipReadAllowed("present"));
  f.set_enabled(false);
  EXPECT_FALSE(f.enabled());
  EXPECT_FALSE(f.SkipReadAllowed("absent"));
  EXPECT_EQ(f.state_name(), "disabled");
}

TEST(SpecialKeyFilter, GrowthAndSaturation) {
  SpecialKeyFilter f(16, 128);
  EXPECT_EQ(f.MaxCapacity(), 102u);
  for (int i = 0; i < 50; ++i) f.Insert("k" + std::to_string(i));
  EXPECT_TRUE(f.NeedsGrowth());
  f.Reset(1000);
  EXPECT_EQ(f.Capacity(), 102u);
  EXPECT_EQ(f.state(), SpecialKeyFilter::State::kBuilding);
  EXPECT_EQ(f.Count(), 0u);
  for (int i = 0; i < 200; ++i) f.Insert("k" + std::to_string(i));
  f.MarkReady();
  EXPECT_EQ(f.state(), SpecialKeyFilter::State::kSaturated);
  EXPECT_EQ(f.state_name(), "saturated");
  EXPECT_FALSE(f.SkipReadAllowed("never-inserted"));
}

TEST(SpecialKeyFilter, ConcurrentInsertAndQuery) {
  SpecialKeyFilter f(size_t{1} << 18);
  constexpr int kPer = 50000;
  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPer; ++i) f.Insert("w" + std::to_string(t) + "/" + std::to_string(i));
    });
  }
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&, t] {
      size_t n = 0;
      while (!stop.load(std::memory_order_relaxed)) n += f.MayContain("w" + std::to_string(t) + "/7") ? 1 : 0;
      (void)n;
    });
  }
  for (auto& th : threads) th.join();
  stop = true;
  for (auto& th : readers) th.join();
  for (int t = 0; t < 4; ++t)
    for (int i = 0; i < kPer; ++i)
      ASSERT_TRUE(f.MayContain("w" + std::to_string(t) + "/" + std::to_string(i)));
}

TEST(SpecialKeyFilter, CheckSpecialKeysCountsMissing) {
  InMemoryStorage storage;
  auto txn = storage.BeginWrite();
  ASSERT_TRUE(txn.ok());
  ASSERT_TRUE((*txn)->Put(ColumnFamily::kMeta, "hashkey", WithType(encoding::ValueType::kHash)).ok());
  ASSERT_TRUE((*txn)->Put(ColumnFamily::kMeta, "strkey", Plain()).ok());
  ASSERT_TRUE((*txn)->Commit().ok());
  SpecialKeyFilter f;
  auto view = storage.BeginRead();
  ASSERT_TRUE(view.ok());
  auto r = CheckSpecialKeys(**view, f);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r->specials, 1u);
  EXPECT_EQ(r->missing, 1u);
  f.Insert("hashkey");
  r = CheckSpecialKeys(**view, f);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r->specials, 1u);
  EXPECT_EQ(r->missing, 0u);
}

}  // namespace
}  // namespace tideskv
