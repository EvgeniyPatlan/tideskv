#include <gtest/gtest.h>

#include "support/command_harness.h"
#include "types/errors.h"

namespace tideskv {
namespace {

using test::Arr;
using test::Bulk;
using test::CommandHarness;
using test::Err;
using test::Int;
using test::kNil;
using test::kOk;

struct ZsetRangeCmds : ::testing::Test {
  void SetUp() override { ASSERT_EQ(h.Run({"ZADD", "z", "1", "a", "2", "b", "3", "c"}), Int(3)); }
  CommandHarness h;
};

TEST_F(ZsetRangeCmds, UnifiedZrange) {
  EXPECT_EQ(h.Run({"ZRANGE", "z", "0", "-1"}), Arr({Bulk("a"), Bulk("b"), Bulk("c")}));
  EXPECT_EQ(h.Run({"ZRANGE", "z", "0", "0", "WITHSCORES"}), Arr({Bulk("a"), Bulk("1")}));
  EXPECT_EQ(h.Run({"ZRANGE", "z", "0", "0", "REV"}), Arr({Bulk("c")}));
  EXPECT_EQ(h.Run({"ZRANGE", "z", "(1", "+inf", "BYSCORE"}), Arr({Bulk("b"), Bulk("c")}));
  EXPECT_EQ(h.Run({"ZRANGE", "z", "+inf", "(1", "BYSCORE", "REV", "LIMIT", "0", "1"}), Arr({Bulk("c")}));
  EXPECT_EQ(h.Run({"ZRANGE", "z", "0", "-1", "LIMIT", "5", "-1"}),
            Arr({Bulk("a"), Bulk("b"), Bulk("c")}));  // quirk
  EXPECT_EQ(h.Run({"ZRANGE", "nokey", "0", "-1"}), "*0\r\n");
}

TEST_F(ZsetRangeCmds, ZrangeErrors) {
  EXPECT_EQ(h.Run({"ZRANGE", "z", "0", "-1", "LIMIT", "0", "1"}), Err(kErrZrangeLimit));
  EXPECT_EQ(h.Run({"ZRANGE", "z", "[a", "[b", "BYLEX", "WITHSCORES"}), Err(kErrZrangeWithscoresLex));
  EXPECT_EQ(h.Run({"ZRANGE", "z", "0", "-1", "REV", "REV"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"ZRANGE", "z", "0", "-1", "BYSCORE", "BYLEX"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"ZRANGE", "z", "0", "-1", "LIMIT", "0"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"ZRANGE", "z", "x", "-1"}), Err(kErrNotInteger));
  EXPECT_EQ(h.Run({"ZRANGE", "z", "x", "1", "BYSCORE"}), Err(kErrMinMaxNotFloat));
  EXPECT_EQ(h.Run({"SET", "s", "v"}), kOk);
  EXPECT_EQ(h.Run({"ZRANGE", "s", "x", "1"}), Err(kErrNotInteger));  // range before the type
  EXPECT_EQ(h.Run({"ZRANGE", "s", "0", "1"}), Err(kErrWrongType));
}

TEST_F(ZsetRangeCmds, LegacyForms) {
  EXPECT_EQ(h.Run({"ZREVRANGE", "z", "0", "0", "WITHSCORES"}), Arr({Bulk("c"), Bulk("3")}));
  EXPECT_EQ(h.Run({"ZRANGEBYSCORE", "z", "-inf", "2", "LIMIT", "1", "1"}), Arr({Bulk("b")}));
  EXPECT_EQ(h.Run({"ZREVRANGEBYSCORE", "z", "2", "-inf"}), Arr({Bulk("b"), Bulk("a")}));
  EXPECT_EQ(h.Run({"ZRANGEBYSCORE", "z", "0", "5", "REV"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"ZADD", "l", "0", "a", "0", "b", "0", "c"}), Int(3));
  EXPECT_EQ(h.Run({"ZRANGEBYLEX", "l", "-", "[b"}), Arr({Bulk("a"), Bulk("b")}));
  EXPECT_EQ(h.Run({"ZREVRANGEBYLEX", "l", "+", "(a"}), Arr({Bulk("c"), Bulk("b")}));
  EXPECT_EQ(h.Run({"ZRANGEBYLEX", "l", "-", "+", "LIMIT", "0", "0"}), "*0\r\n");
  EXPECT_EQ(h.Run({"ZRANGEBYLEX", "l", "-", "+", "WITHSCORES"}), Err(kErrZrangeWithscoresLex));
}

TEST_F(ZsetRangeCmds, Resp3WithScoresShape) {
  h.Resp3();
  EXPECT_EQ(h.Run({"ZRANGE", "z", "0", "1", "WITHSCORES"}),
            "*2\r\n*2\r\n" + Bulk("a") + ",1\r\n*2\r\n" + Bulk("b") + ",2\r\n");
}

TEST_F(ZsetRangeCmds, Rank) {
  EXPECT_EQ(h.Run({"ZRANK", "z", "b"}), Int(1));
  EXPECT_EQ(h.Run({"ZREVRANK", "z", "b"}), Int(1));
  EXPECT_EQ(h.Run({"ZRANK", "z", "c", "WITHSCORE"}), Arr({Int(2), Bulk("3")}));
  EXPECT_EQ(h.Run({"ZRANK", "z", "zz"}), kNil);
  EXPECT_EQ(h.Run({"ZRANK", "nokey", "a", "WITHSCORE"}), "*-1\r\n");
  EXPECT_EQ(h.Run({"ZRANK", "z", "a", "WITHSCORES"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"ZRANK", "z", "a", "WITHSCORE", "x"}),
            Err("ERR wrong number of arguments for 'zrank' command"));
  h.Resp3();
  EXPECT_EQ(h.Run({"ZREVRANK", "z", "a", "WITHSCORE"}), Arr({Int(2), ",1\r\n"}));
}

TEST_F(ZsetRangeCmds, RangeStore) {
  EXPECT_EQ(h.Run({"SET", "dst", "v"}), kOk);
  EXPECT_EQ(h.Run({"ZRANGESTORE", "dst", "z", "1", "-1"}), Int(2));
  EXPECT_EQ(h.Run({"ZRANGE", "dst", "0", "-1", "WITHSCORES"}),
            Arr({Bulk("b"), Bulk("2"), Bulk("c"), Bulk("3")}));
  EXPECT_EQ(h.Run({"ZRANGESTORE", "dst", "nokey", "0", "-1"}), Int(0));
  EXPECT_EQ(h.Run({"EXISTS", "dst"}), Int(0));
  EXPECT_EQ(h.Run({"ZRANGESTORE", "dst", "z", "0", "-1", "WITHSCORES"}), Err(kErrSyntax));
  EXPECT_EQ(h.Run({"ZRANGESTORE", "z", "z", "0", "0"}), Int(1));  // dst == src
  EXPECT_EQ(h.Run({"ZRANGE", "z", "0", "-1"}), Arr({Bulk("a")}));
}

}  // namespace
}  // namespace tideskv
