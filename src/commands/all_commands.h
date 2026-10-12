#pragma once
#include <string_view>

#include "commands/registry.h"

namespace tideskv {

// Version we claim in HELLO/INFO so client libraries enable Redis 7 behaviour.
inline constexpr std::string_view kRedisCompatVersion = "7.2.0";

void RegisterConnectionCommands(CommandRegistry& registry);  // cmd_connection.cc
void RegisterServerCommands(CommandRegistry& registry);      // cmd_server.cc (Task 17)
void RegisterStringCommands(CommandRegistry& registry);      // cmd_string.cc
void RegisterStringEditCommands(CommandRegistry& registry);  // cmd_string_edit.cc
void RegisterAllCommands(CommandRegistry& registry);

}  // namespace tideskv
