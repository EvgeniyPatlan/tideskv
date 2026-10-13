#include "types/zset_range.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>

#include "types/errors.h"

namespace tideskv {
namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

TEST(ScoreRange, ParsesInclusiveExclusiveAndInf) {
  ScoreRange r;
  ASSERT_TRUE(ParseScoreRange("(1", "+inf", &r).ok());
  EXPECT_EQ(r.min, 1);
  EXPECT_TRUE(r.minex);
  EXPECT_EQ(r.max, kInf);
  EXPECT_FALSE(r.maxex);
  EXPECT_FALSE(ScoreInRange(1, r));
  EXPECT_TRUE(ScoreInRange(1.5, r));
  EXPECT_TRUE(ScoreInRange(kInf, r));
}

TEST(ScoreRange, LooserThanScoreParsing) {  // zslParseRange (redis72-zset.md §0.5)
  ScoreRange r;
  ASSERT_TRUE(ParseScoreRange("", "(", &r).ok());  // both 0
  EXPECT_EQ(r.min, 0);
  EXPECT_TRUE(r.maxex);
  EXPECT_TRUE(ScoreRangeEmpty(r));  // [0, 0) is empty
  ASSERT_TRUE(ParseScoreRange(" 5", "1e999", &r).ok());
  EXPECT_EQ(r.min, 5);
  EXPECT_EQ(r.max, kInf);
  EXPECT_EQ(ParseScoreRange("nan", "1", &r).message(), kErrMinMaxNotFloat);
  EXPECT_EQ(ParseScoreRange("1", "2 ", &r).message(), kErrMinMaxNotFloat);
  EXPECT_EQ(ParseScoreRange("((1", "2", &r).message(), kErrMinMaxNotFloat);
  EXPECT_EQ(ParseScoreRange("a", "b", &r).message(), kErrMinMaxNotFloat);
}

TEST(ScoreRange, EmptyRanges) {
  ScoreRange r;
  ASSERT_TRUE(ParseScoreRange("2", "1", &r).ok());
  EXPECT_TRUE(ScoreRangeEmpty(r));
  ASSERT_TRUE(ParseScoreRange("1", "(1", &r).ok());
  EXPECT_TRUE(ScoreRangeEmpty(r));
  ASSERT_TRUE(ParseScoreRange("1", "1", &r).ok());
  EXPECT_FALSE(ScoreRangeEmpty(r));
}

TEST(LexRange, GrammarAndCompare) {
  LexRange r;
  ASSERT_TRUE(ParseLexRange("[b", "(d", &r).ok());
  EXPECT_FALSE(LexAboveMin("a", r));
  EXPECT_TRUE(LexAboveMin("b", r));
  EXPECT_TRUE(LexBelowMax("c", r));
  EXPECT_FALSE(LexBelowMax("d", r));
  ASSERT_TRUE(ParseLexRange("-", "+", &r).ok());
  EXPECT_TRUE(LexAboveMin("", r));
  EXPECT_TRUE(LexBelowMax(std::string("\xff\xff", 2), r));
  ASSERT_TRUE(ParseLexRange("[+", "[-", &r).ok());  // literal one-byte members "+" and "-"
  EXPECT_TRUE(LexAboveMin("+", r));
  ASSERT_TRUE(ParseLexRange("(", "[", &r).ok());  // the empty string
  EXPECT_TRUE(LexRangeEmpty(r));                  // ("", ""] is empty
  for (const char* bad : {"", "a", "+x", "-x"}) {
    EXPECT_EQ(ParseLexRange(bad, "+", &r).message(), kErrMinMaxNotString) << bad;
  }
}

TEST(LexRange, EmptyRanges) {
  LexRange r;
  ASSERT_TRUE(ParseLexRange("[c", "[a", &r).ok());
  EXPECT_TRUE(LexRangeEmpty(r));
  ASSERT_TRUE(ParseLexRange("+", "-", &r).ok());
  EXPECT_TRUE(LexRangeEmpty(r));
  ASSERT_TRUE(ParseLexRange("[a", "[a", &r).ok());
  EXPECT_FALSE(LexRangeEmpty(r));
  ASSERT_TRUE(ParseLexRange("[a", "(a", &r).ok());
  EXPECT_TRUE(LexRangeEmpty(r));
}

}  // namespace
}  // namespace tideskv
