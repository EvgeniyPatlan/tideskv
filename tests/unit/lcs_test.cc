#include "types/lcs.h"

#include <gtest/gtest.h>

#include "commands/reply_util.h"
#include "support/command_harness.h"

namespace tideskv {
namespace {

using test::Arr;
using test::Bulk;
using test::CommandHarness;
using test::Err;
using test::Int;
using test::kOk;

TEST(Lcs, RedisDocExample) {
  auto r = ComputeLcs("ohmytext", "mynewtext", true, 0);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r->length, 6u);
  EXPECT_EQ(r->sequence, "mytext");
  ASSERT_EQ(r->matches.size(), 2u);
  EXPECT_EQ(r->matches[0].a_start, 4u);  // "text"
  EXPECT_EQ(r->matches[0].a_end, 7u);
  EXPECT_EQ(r->matches[0].b_start, 5u);
  EXPECT_EQ(r->matches[0].b_end, 8u);
  EXPECT_EQ(r->matches[0].len, 4u);
  EXPECT_EQ(r->matches[1].a_start, 2u);  // "my"
  EXPECT_EQ(r->matches[1].a_end, 3u);
  EXPECT_EQ(r->matches[1].b_start, 0u);
  EXPECT_EQ(r->matches[1].b_end, 1u);
}

TEST(Lcs, MinMatchLenFiltersRanges) {
  auto r = ComputeLcs("ohmytext", "mynewtext", true, 4);
  ASSERT_TRUE(r.ok());
  ASSERT_EQ(r->matches.size(), 1u);
  EXPECT_EQ(r->matches[0].len, 4u);
}

TEST(Lcs, EmptyInputs) {
  auto r = ComputeLcs("", "abc", true, 0);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r->length, 0u);
  EXPECT_EQ(r->sequence, "");
  EXPECT_TRUE(r->matches.empty());
}

TEST(Lcs, MemoryCap) {
  const std::string big(20000, 'a');
  auto r = ComputeLcs(big, big, true, 0);  // 20001^2*4 bytes > 512 MiB
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.status().message(), kErrLcsMemory);
}

struct LcsCmd : ::testing::Test {
  CommandHarness h;
  std::string Run(std::initializer_list<const char*> a) { return h.Run(ArgList(a.begin(), a.end())); }
  void SetUp() override { ASSERT_EQ(Run({"MSET", "a", "ohmytext", "b", "mynewtext"}), kOk); }
};

TEST_F(LcsCmd, PlainLenAndIdx) {
  EXPECT_EQ(Run({"LCS", "a", "b"}), Bulk("mytext"));
  EXPECT_EQ(Run({"LCS", "a", "b", "LEN"}), Int(6));
  const std::string m1 = Arr({Arr({Int(4), Int(7)}), Arr({Int(5), Int(8)})});
  const std::string m2 = Arr({Arr({Int(2), Int(3)}), Arr({Int(0), Int(1)})});
  EXPECT_EQ(Run({"LCS", "a", "b", "IDX"}), Arr({Bulk("matches"), Arr({m1, m2}), Bulk("len"), Int(6)}));
  const std::string w1 = Arr({Arr({Int(4), Int(7)}), Arr({Int(5), Int(8)}), Int(4)});
  EXPECT_EQ(Run({"LCS", "a", "b", "idx", "minmatchlen", "4", "withmatchlen"}),
            Arr({Bulk("matches"), Arr({w1}), Bulk("len"), Int(6)}));
}

TEST_F(LcsCmd, MissingKeyIsEmptyAndErrors) {
  EXPECT_EQ(Run({"LCS", "a", "nope"}), Bulk(""));
  EXPECT_EQ(Run({"LCS", "a", "b", "LEN", "IDX"}), Err(kErrLcsLenAndIdx));
  EXPECT_EQ(Run({"LCS", "a", "b", "BOGUS"}), Err(kErrSyntax));
  EXPECT_EQ(Run({"LCS", "a", "b", "MINMATCHLEN"}), Err(kErrSyntax));
  EXPECT_EQ(Run({"LCS", "a", "b", "MINMATCHLEN", "x"}), Err(kErrNotInteger));
  encoding::Metadata m;
  m.type = encoding::ValueType::kList;
  m.oid = 1;
  h.SeedMeta(0, "l", m);
  EXPECT_EQ(Run({"LCS", "a", "l"}), Err(kErrLcsNotStrings));
}

}  // namespace
}  // namespace tideskv
