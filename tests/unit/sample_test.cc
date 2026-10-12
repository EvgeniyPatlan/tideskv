#include "types/sample.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <set>

namespace tideskv {
namespace {

TEST(SamplePositions, DistinctIsSortedUniqueAndInRange) {
  std::mt19937_64 rng(42);
  for (uint64_t n : {1u, 5u, 100u}) {
    for (uint64_t k = 0; k <= n; ++k) {
      const auto pos = SamplePositions(n, k, true, rng);
      ASSERT_EQ(pos.size(), k);
      EXPECT_TRUE(std::is_sorted(pos.begin(), pos.end()));
      EXPECT_EQ(std::set<uint64_t>(pos.begin(), pos.end()).size(), k);
      for (uint64_t p : pos) EXPECT_LT(p, n);
    }
  }
}

TEST(SamplePositions, DistinctCountAboveSizeIsClamped) {
  std::mt19937_64 rng(1);
  EXPECT_EQ(SamplePositions(3, 10, true, rng), (std::vector<uint64_t>{0, 1, 2}));
}

TEST(SamplePositions, RepeatsAreSortedAndMayRepeat) {
  std::mt19937_64 rng(7);
  const auto pos = SamplePositions(2, 50, false, rng);
  ASSERT_EQ(pos.size(), 50u);
  EXPECT_TRUE(std::is_sorted(pos.begin(), pos.end()));
  EXPECT_GT(std::count(pos.begin(), pos.end(), 0u), 0);
  EXPECT_GT(std::count(pos.begin(), pos.end(), 1u), 0);
}

TEST(SamplePositions, EmptyInputs) {
  std::mt19937_64 rng(3);
  EXPECT_TRUE(SamplePositions(0, 5, true, rng).empty());
  EXPECT_TRUE(SamplePositions(0, 5, false, rng).empty());
  EXPECT_TRUE(SamplePositions(10, 0, false, rng).empty());
}

TEST(SamplePositions, DistinctIsRoughlyUniform) {
  std::mt19937_64 rng(11);
  std::map<uint64_t, int> hits;
  for (int i = 0; i < 20000; ++i) {
    for (uint64_t p : SamplePositions(10, 3, true, rng)) ++hits[p];
  }
  for (const auto& [p, n] : hits) EXPECT_NEAR(n, 6000, 600) << "position " << p;  // 20000*3/10
}

}  // namespace
}  // namespace tideskv
