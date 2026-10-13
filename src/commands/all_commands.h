#pragma once
#include <string_view>

#include "commands/registry.h"

namespace tideskv {

// Version we claim in HELLO/INFO so client libraries enable Redis 7 behaviour.
inline constexpr std::string_view kRedisCompatVersion = "7.2.0";

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
void RegisterAllCommands(CommandRegistry& registry);

}  // namespace tideskv
