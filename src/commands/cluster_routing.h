#pragma once
#include <optional>
#include <string>

#include "commands/registry.h"

// The routing hook points of cluster mode (M7a D13-D16), between the commands layer and src/cluster/.
namespace tideskv {

class BlockedClient;
class ServerServices;
struct Session;
namespace cluster {
struct Topology;
}  // namespace cluster

// The refusal of one dispatched call (Redis getNodeByQuery in processCommand), or "" to run it here; always
// "" when cluster mode is off. EXEC routes the union of its queue's keys (D14).
std::string ClusterRefusal(ExecContext& ctx, const CommandSpec& spec, const ArgList& argv, bool is_write);

// A redis.call in cluster mode (Redis scriptVerifyClusterState, D15): any routing refusal is a non-local key;
// unless the script allows cross-slot keys, every key must stay in its first slot (*first_slot, -1 = none
// yet).
std::string ScriptClusterRefusal(ServerServices& server, const Session& session, const CommandSpec& spec,
                                 const ArgList& argv, bool allow_cross_slot, int* first_slot);

// The error that ends a blocked client's wait after a topology change, or nullopt to keep waiting (D16):
// its keys share one slot, routed as a write (every blocking command writes). WAIT (no keys) never moves.
std::optional<std::string> BlockedRedirect(const cluster::Topology& t, const BlockedClient& c);

}  // namespace tideskv
