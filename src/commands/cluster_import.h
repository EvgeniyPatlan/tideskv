#pragma once
#include "cluster/topology.h"
#include "commands/registry.h"

// CLUSTER IMPORT <slots> <0|1|2> and CLUSTER IMPORTDATA <chunk> (M7b D3, D14): the target end of a migration.
namespace tideskv {

void ClusterImport(ExecContext& ctx, const cluster::Topology& t, const ArgList& argv);
void ClusterImportData(ExecContext& ctx, const ArgList& argv);

}  // namespace tideskv
