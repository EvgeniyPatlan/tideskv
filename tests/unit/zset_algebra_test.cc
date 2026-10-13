#include "types/zset_algebra.h"

#include <gtest/gtest.h>

#include <limits>

namespace tideskv {
namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

std::vector<std::pair<std::string, double>> Flat(const std::vector<ScoreMember>& v) {
  std::vector<std::pair<std::string, double>> out;
  for (const auto& p : v) out.emplace_back(p.member, p.score);
  return out;
}

TEST(ZsetAlgebra, UnionSumsWithWeights) {
  const std::vector<ZsetSource> src = {{{"a", 1}, {"b", 2}}, {{"b", 3}, {"c", 4}}};
  EXPECT_EQ(Flat(CombineZsets(ZsetOp::kUnion, src, {1, 2}, Aggregate::kSum, false)),
            (std::vector<std::pair<std::string, double>>{{"a", 1}, {"b", 8}, {"c", 8}}));
  EXPECT_EQ(Flat(CombineZsets(ZsetOp::kUnion, src, {1, 1}, Aggregate::kMax, false)),
            (std::vector<std::pair<std::string, double>>{{"a", 1}, {"b", 3}, {"c", 4}}));
}

TEST(ZsetAlgebra, InterAndDiff) {
  const std::vector<ZsetSource> src = {{{"a", 1}, {"b", 2}, {"c", 3}}, {{"b", 10}, {"c", 1}}};
  EXPECT_EQ(Flat(CombineZsets(ZsetOp::kInter, src, {1, 1}, Aggregate::kMin, false)),
            (std::vector<std::pair<std::string, double>>{{"c", 1}, {"b", 2}}));
  EXPECT_EQ(Flat(CombineZsets(ZsetOp::kDiff, src, {1, 1}, Aggregate::kSum, false)),
            (std::vector<std::pair<std::string, double>>{{"a", 1}}));
  EXPECT_TRUE(CombineZsets(ZsetOp::kDiff, src, {1, 1}, Aggregate::kSum, /*first_key_repeated=*/true).empty());
  EXPECT_TRUE(CombineZsets(ZsetOp::kInter, {{{"a", 1}}, {}}, {1, 1}, Aggregate::kSum, false).empty());
}

TEST(ZsetAlgebra, InfinitiesSumToZeroNotNan) {  // Review Focus #5
  const std::vector<ZsetSource> src = {{{"a", kInf}}, {{"a", -kInf}}};
  EXPECT_EQ(Flat(CombineZsets(ZsetOp::kUnion, src, {1, 1}, Aggregate::kSum, false)),
            (std::vector<std::pair<std::string, double>>{{"a", 0}}));
  EXPECT_EQ(Flat(CombineZsets(ZsetOp::kInter, src, {1, 1}, Aggregate::kSum, false)),
            (std::vector<std::pair<std::string, double>>{{"a", 0}}));
  EXPECT_EQ(Flat(CombineZsets(ZsetOp::kUnion, {{{"a", kInf}}}, {0}, Aggregate::kSum, false)),  // inf*0 -> 0
            (std::vector<std::pair<std::string, double>>{{"a", 0}}));
}

TEST(ZsetAlgebra, InterCardinalityWithLimit) {
  const std::vector<ZsetSource> src = {{{"a", 1}, {"b", 1}, {"c", 1}},
                                       {{"a", 1}, {"b", 1}, {"c", 1}, {"d", 1}}};
  EXPECT_EQ(ZsetInterCardinality(src, 0), 3u);
  EXPECT_EQ(ZsetInterCardinality(src, 2), 2u);
  EXPECT_EQ(ZsetInterCardinality({{{"a", 1}}, {}}, 0), 0u);
}

TEST(ZsetAlgebra, ResultOrderIsScoreThenMemberBytes) {
  const std::vector<ZsetSource> src = {{{"b", 1}, {"a", 1}, {std::string("\xff", 1), 0}}};
  EXPECT_EQ(Flat(CombineZsets(ZsetOp::kUnion, src, {1}, Aggregate::kSum, false)),
            (std::vector<std::pair<std::string, double>>{{std::string("\xff", 1), 0}, {"a", 1}, {"b", 1}}));
}

}  // namespace
}  // namespace tideskv
