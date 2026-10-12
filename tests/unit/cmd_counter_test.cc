#include <gtest/gtest.h>

#include "commands/reply_util.h"
#include "support/command_harness.h"
#include "types/errors.h"

namespace tideskv {
namespace {

using test::Bulk;
using test::CommandHarness;
using test::Err;
using test::Int;
using test::kOk;

struct CounterCmds : ::testing::Test {
  CommandHarness h;
};

TEST_F(CounterCmds, IncrDecrFamily) {
  EXPECT_EQ(h.Run({"INCR", "n"}), Int(1));
  EXPECT_EQ(h.Run({"INCRBY", "n", "10"}), Int(11));
  EXPECT_EQ(h.Run({"DECR", "n"}), Int(10));
  EXPECT_EQ(h.Run({"DECRBY", "n", "15"}), Int(-5));
  EXPECT_EQ(h.Run({"GET", "n"}), Bulk("-5"));
  EXPECT_EQ(h.ExpireOf(0, "n"), -1);
}

TEST_F(CounterCmds, StoredValueMustBeCanonicalInteger) {
  for (const char* bad : {"007", " 1", "1 ", "1.5", "abc", "", "123456789012345678901"}) {
    EXPECT_EQ(h.Run({"SET", "n", bad}), kOk);
    EXPECT_EQ(h.Run({"INCR", "n"}), Err(kErrNotInteger)) << bad;
  }
}

TEST_F(CounterCmds, Overflow) {
  EXPECT_EQ(h.Run({"SET", "n", "9223372036854775807"}), kOk);
  EXPECT_EQ(h.Run({"INCR", "n"}), Err(kErrOverflow));
  EXPECT_EQ(h.Run({"SET", "n", "-9223372036854775808"}), kOk);
  EXPECT_EQ(h.Run({"DECR", "n"}), Err(kErrOverflow));
  EXPECT_EQ(h.Run({"DECRBY", "n", "-9223372036854775808"}), Err(kErrDecrementOverflow));
  EXPECT_EQ(h.Run({"INCRBY", "n", "x"}), Err(kErrNotInteger));
  EXPECT_EQ(h.Run({"GET", "n"}), Bulk("-9223372036854775808"));  // unchanged after errors
}

TEST_F(CounterCmds, KeepsTtl) {
  const int64_t now = h.clock().NowMs();
  EXPECT_EQ(h.Run({"SET", "n", "1", "PX", "500"}), kOk);
  EXPECT_EQ(h.Run({"INCR", "n"}), Int(2));
  EXPECT_EQ(h.Run({"INCRBYFLOAT", "n", "0.5"}), Bulk("2.5"));
  EXPECT_EQ(h.ExpireOf(0, "n"), now + 500);
}

TEST_F(CounterCmds, ExpiredValueCountsFromZero) {  // Review Focus #1
  EXPECT_EQ(h.Run({"SET", "n", "41", "PX", "5"}), kOk);
  h.clock().Advance(5);
  EXPECT_EQ(h.Run({"INCR", "n"}), Int(1));
  EXPECT_EQ(h.ExpireOf(0, "n"), -1);  // the stale TTL is not carried over
}

TEST_F(CounterCmds, IncrByFloat) {
  EXPECT_EQ(h.Run({"INCRBYFLOAT", "f", "10.5"}), Bulk("10.5"));
  EXPECT_EQ(h.Run({"INCRBYFLOAT", "f", "0.1"}), Bulk("10.6"));
  EXPECT_EQ(h.Run({"GET", "f"}), Bulk("10.6"));
  EXPECT_EQ(h.Run({"SET", "f", "5.0e3"}), kOk);
  EXPECT_EQ(h.Run({"INCRBYFLOAT", "f", "2.0e2"}), Bulk("5200"));
  EXPECT_EQ(h.Run({"INCRBYFLOAT", "f", "abc"}), Err(kErrNotFloat));
  EXPECT_EQ(h.Run({"SET", "f", "1.5x"}), kOk);
  EXPECT_EQ(h.Run({"INCRBYFLOAT", "f", "1"}), Err(kErrNotFloat));
  EXPECT_EQ(h.Run({"SET", "f", "1"}), kOk);
  EXPECT_EQ(h.Run({"INCRBYFLOAT", "f", "inf"}), Err(kErrNanOrInfinity));
  EXPECT_EQ(h.Run({"GET", "f"}), Bulk("1"));
}

TEST_F(CounterCmds, WrongTypeComesBeforeArgumentChecks) {
  h.SeedHash(0, "s", 3, false);
  EXPECT_EQ(h.Run({"INCR", "s"}), Err(kErrWrongType));
  EXPECT_EQ(h.Run({"INCRBYFLOAT", "s", "1"}), Err(kErrWrongType));
  EXPECT_EQ(h.Run({"INCRBYFLOAT", "s", "abc"}), Err(kErrWrongType));
  EXPECT_EQ(h.Run({"INCRBY", "s", "x"}), Err(kErrNotInteger));  // Redis parses INCRBY's arg first
}

}  // namespace
}  // namespace tideskv
