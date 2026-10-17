#pragma once
#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

#include "cluster/topology.h"
#include "migrate/migration_service.h"
#include "repl/full_sync_frames.h"
#include "support/cluster_harness.h"
#include "types/slot_keys.h"

// Two in-process cluster nodes for migration tests (M7b): A, the source (port 7000), and B, the target
// (7001), both of kThreeNodes at version 1. Task 10 adds the in-process transport between them.
namespace tideskv::test {

// The snapshot chunks of `range` on `source`, as its migrator sends them (D2): meta, subkey and zscore of the
// range, no expire CF, no sys record.
inline std::vector<std::string> RangeChunks(CommandHarness& source, cluster::SlotRange range,
                                            size_t max_bytes = 16 * 1024) {
  auto snap = source.services.repl_log_.SnapshotAtLast();
  EXPECT_TRUE(snap.ok());
  if (!snap.ok()) return {};
  repl::ChunkerOptions o;
  o.cfs = {ColumnFamily::kMeta, ColumnFamily::kSubkey, ColumnFamily::kZScore};
  o.bounds = SlotRangeBounds(range.start, range.end);
  o.sys_keys = {};
  o.max_bytes = max_bytes;
  repl::SnapshotChunker chunker(*snap->view, 0, o);
  std::vector<std::string> out;
  for (;;) {
    auto next = chunker.Next(nullptr);
    EXPECT_TRUE(next.ok());
    if (!next.ok() || !next->has_value()) return out;
    out.push_back(**next);
  }
}

struct MigrationPair {
  MigrationPair() {
    source_cluster.Push(kThreeNodes, 1);
    target_cluster.Push(kThreeNodes, 1);
  }
  migrate::MigrationService& source_migration() { return *source_cluster.migration; }
  migrate::MigrationService& target_migration() { return *target_cluster.migration; }

  CommandHarness source;
  CommandHarness target;
  ClusterHarness source_cluster{source, 7000};
  ClusterHarness target_cluster{target, 7001};
};

}  // namespace tideskv::test
