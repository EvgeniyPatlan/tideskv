#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "encoding/key_codec.h"
#include "encoding/meta_codec.h"
#include "exec/engine.h"
#include "storage/memory_storage.h"
#include "support/command_harness.h"
#include "support/replica_stack.h"
#include "types/errors.h"

namespace tideskv::test {
namespace {

using encoding::ReplOp;
using encoding::ReplOpType;

std::vector<ReplOp> PutB(const std::string& value) {
  return {{ReplOpType::kPut, ColumnFamily::kMeta, encoding::MetaKeyFor(0, "b"),
           encoding::EncodeMetadata({}, value)}};
}

TEST(EngineImport, ApplyImportCommitsThroughTheReplLogAndReplicates) {  // D22
  CommandHarness h;
  InMemoryStorage replica_storage;
  ReplicaStack replica(replica_storage);
  const uint64_t before = h.services.repl_log_.last();
  ASSERT_TRUE(h.services.engine_.ApplyImport(PutB("v")).ok());
  EXPECT_EQ(h.services.repl_log_.last(), before + 1);
  EXPECT_EQ(h.Run({"GET", "b"}), Bulk("v"));
  ASSERT_TRUE(ShipAll(h.services.storage_, h.services.repl_log_.last(), replica.engine).ok());
  EXPECT_EQ(DigestOf(replica.engine), DigestOf(h.services.engine_));
  EXPECT_TRUE(h.services.engine_.ApplyImport({}).ok());  // nothing to commit
  EXPECT_EQ(h.services.repl_log_.last(), before + 1);
}

TEST(EngineImport, AFencedLogRefusesAndNothingIsApplied) {  // a demoted target refuses (D7, D22)
  CommandHarness h;
  h.services.repl_log_.FenceLocalWrites(true);
  EXPECT_EQ(h.services.engine_.ApplyImport(PutB("v")).message(), kErrReadOnlyReplica);
  EXPECT_EQ(h.services.engine_.WriteInternal(PutB("v")).message(), kErrReadOnlyReplica);
  h.services.repl_log_.FenceLocalWrites(false);
  EXPECT_TRUE(h.Cf(ColumnFamily::kMeta).empty());
}

TEST(EngineImport, InternalWritesBypassTheSlotFence) {  // D4: unfenced paths
  CommandHarness h;
  h.services.engine_.FenceSlots(3300, 3300);
  EXPECT_TRUE(h.services.engine_.ApplyImport(PutB("v")).ok());
  EXPECT_TRUE(
      h.services.engine_
          .WriteInternal({{ReplOpType::kDelete, ColumnFamily::kMeta, encoding::MetaKeyFor(0, "b"), ""}})
          .ok());
  EXPECT_TRUE(h.Cf(ColumnFamily::kMeta).empty());
}

TEST(EngineImport, AWatchedKeyThatAnImportWritesAbortsTheTransaction) {  // reported like RunTxn
  CommandHarness h;
  ASSERT_EQ(h.Run({"WATCH", "b"}), kOk);
  ASSERT_TRUE(h.services.engine_.ApplyImport(PutB("v")).ok());
  ASSERT_EQ(h.Run({"MULTI"}), kOk);
  ASSERT_EQ(h.Run({"GET", "b"}), "+QUEUED\r\n");
  EXPECT_EQ(h.Run({"EXEC"}), "*-1\r\n");
}

TEST(EngineImport, InternalWritesAreRefusedWhileLoadingAndInsideABatch) {
  CommandHarness h;
  {
    auto batch = h.services.engine_.BeginBatch();
    ASSERT_TRUE(batch.ok());
    EXPECT_EQ(h.services.engine_.ApplyImport(PutB("v")).code(), StatusCode::kInternal);
  }
  InMemoryStorage replica_storage;
  ReplicaStack replica(replica_storage);
  ASSERT_TRUE(replica.engine.BeginLoad({}).ok());
  EXPECT_EQ(replica.engine.WriteInternal(PutB("v")).message(), kErrLoading);
}

}  // namespace
}  // namespace tideskv::test
