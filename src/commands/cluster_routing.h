#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "commands/registry.h"

// The routing hook points of cluster mode (M7a D13-D16), between the commands layer and src/cluster/.
namespace tideskv {

class BlockedClient;
class ServerServices;
struct Session;
namespace cluster {
struct SlotStates;
struct Topology;
}  // namespace cluster

// The keys of one dispatched call, as views into argv; EXEC: the union of its queue's keys (D14).
std::vector<std::string_view> CallKeys(ExecContext& ctx, const CommandSpec& spec, const ArgList& argv);

// The refusal of one dispatched call (Redis getNodeByQuery in processCommand), or "" to run it here; always
// "" when cluster mode is off. EXEC routes the union of its queue's keys (D14).
std::string ClusterRefusal(ExecContext& ctx, const CommandSpec& spec, const ArgList& argv, bool is_write);

// M7b D4, after BeginBatch (the exclusive gate pins the fence): kErrTryAgain when the call writes and its
// keys' slot is fenced, so an EXEC or EVAL fails as a whole with nothing applied; "" otherwise and outside
// cluster mode. Dispatch refused CROSSSLOT already, so the first key's slot is the call's.
std::string FencedCallRefusal(ExecContext& ctx, const std::vector<std::string_view>& keys, bool writes);

// A redis.call in cluster mode (Redis scriptVerifyClusterState, D15): any routing refusal is a non-local key,
// except a TRYAGAIN, passed through (M7b D4: a fenced slot is local);
// unless the script allows cross-slot keys, every key must stay in its first slot (*first_slot, -1 = none
// yet).
std::string ScriptClusterRefusal(ServerServices& server, const Session& session, const CommandSpec& spec,
                                 const ArgList& argv, bool allow_cross_slot, int* first_slot);

// D16 + M7b D18: the error that ends a blocked client's wait after a topology or states publish, or nullopt
// to keep waiting. Its keys share one slot, routed as a write (every blocking command writes) with the wait's
// own READONLY and ASKING flags; a TRYAGAIN keeps the wait (a fenced slot until the success publish MOVEs it;
// an ASKING wait on an importing slot until it is imported). WAIT (no keys) never moves.
std::optional<std::string> BlockedRedirect(const cluster::Topology& t, const cluster::SlotStates& s,
                                           const BlockedClient& c);
// The M7a form: empty migration states.
std::optional<std::string> BlockedRedirect(const cluster::Topology& t, const BlockedClient& c);

}  // namespace tideskv
