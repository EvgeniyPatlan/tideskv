#include <gtest/gtest.h>

#include <string>

#include "encoding/key_codec.h"
#include "support/command_harness.h"
#include "types/object_store.h"

namespace tideskv::test {
namespace {

std::string Hex(const std::string& reply) { return reply.substr(5, 40); }  // "*4\r\n+" then the hex

TEST(DebugDigestIndex, AnEmptyDatasetIsZeros) {
  CommandHarness h;
  EXPECT_EQ(h.Run({"DEBUG", "DIGEST-INDEX"}),
            Arr({"+" + std::string(40, '0') + "\r\n", Int(0), Int(0), Int(0)}));
}

// What DEBUG DIGEST ignores (Redis mixes a constant per TTL): the exact deadline.
TEST(DebugDigestIndex, TracksTheExactDeadlineThatDebugDigestIgnores) {
  CommandHarness h;
  ASSERT_EQ(h.Run({"SET", "k", "v", "PX", "100000"}), kOk);
  const std::string digest = h.Run({"DEBUG", "DIGEST"});
  const std::string index = h.Run({"DEBUG", "DIGEST-INDEX"});
  EXPECT_EQ(index.substr(index.size() - 12), ":1\r\n:0\r\n:0\r\n") << index;
  ASSERT_EQ(h.Run({"PEXPIRE", "k", "200000"}), Int(1));
  EXPECT_EQ(h.Run({"DEBUG", "DIGEST"}), digest);
  const std::string after = h.Run({"DEBUG", "DIGEST-INDEX"});
  EXPECT_NE(Hex(after), Hex(index));
  EXPECT_EQ(after.substr(after.size() - 12), ":1\r\n:0\r\n:0\r\n");
}

TEST(DebugDigestIndex, CountsAMissingEntryAndAnOrphan) {
  CommandHarness h;
  ASSERT_EQ(h.Run({"SET", "k", "v", "PX", "100000"}), kOk);
  const std::string entry = h.Cf(ColumnFamily::kExpire).begin()->first;
  {
    auto txn = h.services.storage_.BeginWrite();
    ASSERT_TRUE(txn.ok());
    ASSERT_TRUE((*txn)->Delete(ColumnFamily::kExpire, entry).ok());  // the index lost k's entry
    ASSERT_TRUE(
        (*txn)->Put(ColumnFamily::kExpire, encoding::ExpireKeyFor(0, 5, "ghost"), ExpireValue(0)).ok());
    ASSERT_TRUE((*txn)->Commit().ok());
  }
  const std::string r = h.Run({"DEBUG", "DIGEST-INDEX"});
  EXPECT_EQ(r.substr(r.size() - 12), ":1\r\n:1\r\n:1\r\n")
      << r;  // 1 entry (ghost), k missing, ghost orphaned
}

TEST(DebugDigestIndex, IsAvailableWhileLoadingAndListedInHelp) {
  CommandHarness h;
  EXPECT_NE(h.Run({"DEBUG", "HELP"}).find("DIGEST-INDEX"), std::string::npos);
  EXPECT_EQ(h.Run({"DEBUG", "DIGEST-INDEX", "x"}),
            Err("ERR unknown subcommand or wrong number of arguments for 'DIGEST-INDEX'. Try DEBUG HELP."));
}

}  // namespace
}  // namespace tideskv::test
