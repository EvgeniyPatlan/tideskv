#include "common/glob.h"

#include <gtest/gtest.h>

#include <chrono>

namespace tideskv {
namespace {

TEST(Glob, RedisDocExamples) {
  EXPECT_TRUE(GlobMatch("h?llo", "hello"));
  EXPECT_TRUE(GlobMatch("h?llo", "hallo"));
  EXPECT_FALSE(GlobMatch("h?llo", "hllo"));
  EXPECT_TRUE(GlobMatch("h*llo", "hllo"));
  EXPECT_TRUE(GlobMatch("h*llo", "heeeello"));
  EXPECT_TRUE(GlobMatch("h[ae]llo", "hello"));
  EXPECT_TRUE(GlobMatch("h[ae]llo", "hallo"));
  EXPECT_FALSE(GlobMatch("h[ae]llo", "hillo"));
  EXPECT_TRUE(GlobMatch("h[^e]llo", "hallo"));
  EXPECT_FALSE(GlobMatch("h[^e]llo", "hello"));
  EXPECT_TRUE(GlobMatch("h[a-b]llo", "hbllo"));
  EXPECT_FALSE(GlobMatch("h[a-b]llo", "hcllo"));
}

TEST(Glob, StarEdgeCases) {
  EXPECT_TRUE(GlobMatch("*", ""));
  EXPECT_TRUE(GlobMatch("*", "anything"));
  EXPECT_TRUE(GlobMatch("**a**", "xxa"));
  EXPECT_FALSE(GlobMatch("a*", ""));
  EXPECT_TRUE(GlobMatch("", ""));
  EXPECT_FALSE(GlobMatch("", "a"));
}

TEST(Glob, EscapesAndReversedRange) {
  EXPECT_TRUE(GlobMatch("h\\*llo", "h*llo"));
  EXPECT_FALSE(GlobMatch("h\\*llo", "hello"));
  EXPECT_TRUE(GlobMatch("[z-a]", "m"));  // reversed range is normalised
  EXPECT_TRUE(GlobMatch("[\\]]", "]"));
}

TEST(Glob, RangeToCloseBracket) {
  EXPECT_TRUE(GlobMatch("[a-]", "^"));   // range from 'a' (97) to ']' (93), reversed to ']'..'^'
  EXPECT_FALSE(GlobMatch("[a-]", "-"));  // '-' (45) not in range
}

TEST(Glob, NoCase) {
  EXPECT_TRUE(GlobMatch("HeLLo*", "hello world", true));
  EXPECT_FALSE(GlobMatch("HeLLo*", "hello world", false));
  EXPECT_TRUE(GlobMatch("[A-C]x", "bx", true));
}

TEST(Glob, BinarySafe) { EXPECT_TRUE(GlobMatch(std::string_view("a\0*", 3), std::string_view("a\0zz", 4))); }

TEST(Glob, PathologicalPatternIsFast) {
  const std::string str(200, 'a');
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(GlobMatch("a*a*a*a*a*a*a*a*a*a*a*b", str));
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(100));
}

}  // namespace
}  // namespace tideskv
