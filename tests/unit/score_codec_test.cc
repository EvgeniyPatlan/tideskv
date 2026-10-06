#include "encoding/score_codec.h"

#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

namespace tideskv::encoding {
namespace {

TEST(ScoreCodec, IsEightBytes) { EXPECT_EQ(EncodeScore(1.5).size(), 8u); }

TEST(ScoreCodec, NegativeZeroEqualsZero) {
  EXPECT_EQ(EncodeScore(-0.0), EncodeScore(0.0));
  EXPECT_FALSE(std::signbit(DecodeScore(EncodeScore(-0.0))));
}

TEST(ScoreCodec, KnownOrdering) {
  const double inf = std::numeric_limits<double>::infinity();
  const std::vector<double> ascending = {-inf,   -1e300, -1.0, -0.5,  -1e-300, 0.0,
                                         1e-300, 0.5,    1.0,  1e300, inf};
  for (size_t i = 1; i < ascending.size(); ++i) {
    EXPECT_LT(EncodeScore(ascending[i - 1]), EncodeScore(ascending[i])) << ascending[i];
  }
}

RC_GTEST_PROP(ScoreCodecProp, PreservesOrder, (double a, double b)) {
  RC_PRE(!std::isnan(a) && !std::isnan(b));
  if (a == b) {
    RC_ASSERT(EncodeScore(a) == EncodeScore(b));  // also covers -0.0 == +0.0
  } else {
    RC_ASSERT((a < b) == (EncodeScore(a) < EncodeScore(b)));
  }
}

RC_GTEST_PROP(ScoreCodecProp, RoundTrips, (double a)) {
  RC_PRE(!std::isnan(a));
  RC_ASSERT(DecodeScore(EncodeScore(a)) == a);
}

}  // namespace
}  // namespace tideskv::encoding
