#pragma once
#include <string>

#include "commands/registry.h"

// The routing hook points of cluster mode (M7a D13-D16), between the commands layer and src/cluster/.
namespace tideskv {

// The refusal of one dispatched call (Redis getNodeByQuery in processCommand), or "" to run it here; always
// "" when cluster mode is off. EXEC routes the union of its queue's keys (D14).
std::string ClusterRefusal(ExecContext& ctx, const CommandSpec& spec, const ArgList& argv, bool is_write);

}  // namespace tideskv
