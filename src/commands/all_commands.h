#pragma once
#include <string>
#include <string_view>

#include "commands/registry.h"
#include "common/version.h"

namespace tideskv {

void RegisterConnectionCommands(CommandRegistry& registry);    // cmd_connection.cc
void RegisterServerCommands(CommandRegistry& registry);        // cmd_server.cc (Task 17)
void RegisterStringCommands(CommandRegistry& registry);        // cmd_string.cc
void RegisterStringEditCommands(CommandRegistry& registry);    // cmd_string_edit.cc
void RegisterCounterCommands(CommandRegistry& registry);       // cmd_counter.cc
void RegisterLcsCommand(CommandRegistry& registry);            // cmd_lcs.cc
void RegisterKeyspaceCommands(CommandRegistry& registry);      // cmd_keyspace.cc
void RegisterExpireCommands(CommandRegistry& registry);        // cmd_expire.cc
void RegisterScanCommands(CommandRegistry& registry);          // cmd_scan.cc
void RegisterHashCommands(CommandRegistry& registry);          // cmd_hash.cc
void RegisterElementScanCommands(CommandRegistry& registry);   // cmd_element_scan.cc
void RegisterSetCommands(CommandRegistry& registry);           // cmd_set.cc
void RegisterSetAlgebraCommands(CommandRegistry& registry);    // cmd_set_algebra.cc
void RegisterListCommands(CommandRegistry& registry);          // cmd_list.cc
void RegisterListEditCommands(CommandRegistry& registry);      // cmd_list_edit.cc
void RegisterBlockingListCommands(CommandRegistry& registry);  // cmd_blocking_list.cc
void RegisterZsetCommands(CommandRegistry& registry);          // cmd_zset.cc
void RegisterZsetRangeCommands(CommandRegistry& registry);     // cmd_zset_range.cc
void RegisterZsetPopCommands(CommandRegistry& registry);       // cmd_zset_pop.cc
void RegisterZsetAlgebraCommands(CommandRegistry& registry);   // cmd_zset_algebra.cc
void RegisterBlockingZsetCommands(CommandRegistry& registry);  // cmd_blocking_zset.cc
void RegisterMultiCommands(CommandRegistry& registry);         // cmd_multi.cc
void RegisterScriptCommands(CommandRegistry& registry);        // cmd_script.cc (M5b)
void RegisterDebugCommands(CommandRegistry& registry);         // cmd_debug.cc (M6a)
void RegisterReplicationCommands(CommandRegistry& registry);   // cmd_replication.cc (M6a)
void RegisterWaitCommand(CommandRegistry& registry);           // cmd_wait.cc (M6b)
void RegisterClusterxCommand(CommandRegistry& registry);       // cmd_clusterx.cc (M7a)
// Redis rejects an unknown or mis-sized SCRIPT subcommand at lookup time, before MULTI queues it.
// Returns the error text, or empty when `argv` (size >= 2) names a valid subcommand.
std::string ScriptSubcommandError(const ArgList& argv);
void RegisterAllCommands(CommandRegistry& registry);

}  // namespace tideskv
