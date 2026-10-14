#include <gtest/gtest.h>

#include <string>

#include "support/command_harness.h"
#include "types/errors.h"

// R12 (F2): a write whose replicated batch would exceed the replication limit fails on the master with
// an error and changes nothing, whatever produced it (one command, EXEC or a script).
namespace tideskv::test {
namespace {

struct BatchLimitHarness : CommandHarness {
  BatchLimitHarness() { services.repl_log_.SetMaxBatchBytes(256); }
  uint64_t last() const { return services.repl_log_.last(); }
};

TEST(ReplBatchLimit, AnOversizedWriteIsRefusedAndChangesNothing) {
  BatchLimitHarness h;
  EXPECT_EQ(h.Run({"SET", "k", "small"}), kOk);
  const uint64_t last = h.last();
  const std::string big(300, 'x');
  EXPECT_EQ(h.Run({"SET", "k", big}), Err(kErrTooLargeToReplicate));
  EXPECT_EQ(h.Run({"MSET", "a", big, "b", "v"}), Err(kErrTooLargeToReplicate));
  EXPECT_EQ(h.last(), last);
  EXPECT_EQ(h.Run({"GET", "k"}), Bulk("small"));
  EXPECT_EQ(h.Run({"EXISTS", "a", "b"}), Int(0));
  EXPECT_EQ(h.Run({"SET", "k", "next"}), kOk);
  EXPECT_EQ(h.last(), last + 1);
}

TEST(ReplBatchLimit, AnOversizedExecOrScriptIsRefusedAndChangesNothing) {
  BatchLimitHarness h;
  const uint64_t last = h.last();
  const std::string half(150, 'x');
  EXPECT_EQ(h.Run({"MULTI"}), kOk);
  EXPECT_EQ(h.Run({"SET", "a", half}), "+QUEUED\r\n");
  EXPECT_EQ(h.Run({"SET", "b", half}), "+QUEUED\r\n");
  EXPECT_EQ(h.Run({"EXEC"}), Err(kErrTooLargeToReplicate));
  EXPECT_EQ(
      h.Run({"EVAL", "redis.call('SET', 'c', ARGV[1]) redis.call('SET', 'd', ARGV[1]) return 1", "0", half}),
      Err(kErrTooLargeToReplicate));
  EXPECT_EQ(h.Run({"EXISTS", "a", "b", "c", "d"}), Int(0));
  EXPECT_EQ(h.last(), last);
}

}  // namespace
}  // namespace tideskv::test
