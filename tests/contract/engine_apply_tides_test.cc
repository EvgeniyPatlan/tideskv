#include <gtest/gtest.h>

#include <string>

#include "storage/tides_storage.h"
#include "support/command_harness.h"
#include "support/replica_stack.h"
#include "support/temp_dir.h"

namespace tideskv {
namespace {

using test::CommandHarness;
using test::DigestOf;
using test::ReplicaStack;
using test::ShipAll;
using test::TempDir;

// M6a D11 + D2: a master EXEC deleting 40 hashes is one replog batch of more than TidesDB's 32
// range deletes per commit; a TidesStorage replica must still apply it atomically.
TEST(EngineApplyTides, ApplyOfOversizedRangeDeleteBatchCommits) {
  CommandHarness master;
  for (int i = 0; i < 40; ++i) {
    ASSERT_EQ(master.Run({"HSET", "h" + std::to_string(i), "f", "v"}), test::Int(1));
  }
  ASSERT_EQ(master.Run({"MULTI"}), test::kOk);
  for (int i = 0; i < 40; ++i) master.Run({"DEL", "h" + std::to_string(i)});
  ASSERT_EQ(master.Run({"EXEC"}).substr(0, 4), "*40\r");
  ASSERT_EQ(master.Run({"DBSIZE"}), test::Int(0));

  TempDir dir("tideskv-apply");
  StorageOptions opts;
  opts.path = (dir.path() / "db").string();
  opts.sync_mode = SyncMode::kNone;
  auto storage = TidesStorage::Open(opts);
  ASSERT_TRUE(storage.ok()) << storage.status().ToString();
  ReplicaStack replica(**storage);
  const uint64_t last = master.services.repl_log_.last();
  const Status st = ShipAll(master.services.storage_, last, replica.engine);
  ASSERT_TRUE(st.ok()) << st.ToString();
  EXPECT_EQ(replica.log.last(), last);
  EXPECT_EQ(DigestOf(replica.engine), std::string(40, '0'));
  EXPECT_EQ(DigestOf(replica.engine), DigestOf(master.services.engine_));
}

}  // namespace
}  // namespace tideskv
