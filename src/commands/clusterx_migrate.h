#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "cluster/topology.h"
#include "commands/registry.h"

// CLUSTERX MIGRATE (async and SYNC) and CLUSTERX FLUSHSLOTS (M7b D12, D13), after cmd_clusterx.cc's Kvrocks
// parse.
namespace tideskv {

void RunMigrate(ExecContext& ctx, const std::vector<cluster::SlotRange>& slots, const std::string& dst_id,
                bool sync, int64_t timeout_s);
void RunFlushSlots(ExecContext& ctx, const std::vector<cluster::SlotRange>& slots);

}  // namespace tideskv
