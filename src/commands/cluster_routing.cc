#include "commands/cluster_routing.h"

#include <string_view>
#include <vector>

#include "cluster/cluster_errors.h"
#include "cluster/cluster_manager.h"
#include "cluster/slot_router.h"
#include "commands/key_extraction.h"
#include "commands/server_services.h"
#include "server/blocking.h"

namespace tideskv {
namespace {

void AppendKeys(const CommandSpec& spec, const ArgList& argv, std::vector<std::string_view>& keys) {
  for (size_t i : KeyIndexes(spec, argv)) keys.emplace_back(argv[i]);
}

}  // namespace

std::string ClusterRefusal(ExecContext& ctx, const CommandSpec& spec, const ArgList& argv, bool is_write) {
  cluster::ClusterManager* cm = ctx.server.cluster();
  if (cm == nullptr) return {};
  std::vector<std::string_view> keys;
  if (spec.name == "exec") {
    if (!ctx.session.multi.active) return {};  // EXEC answers "EXEC without MULTI" itself
    for (const ArgList& q : ctx.session.multi.queued) {
      if (const CommandSpec* s = ctx.server.registry().Find(q[0]); s != nullptr) AppendKeys(*s, q, keys);
    }
  } else {
    AppendKeys(spec, argv, keys);
  }
  if (keys.empty()) return {};
  return cluster::Route(*cm->Current(), keys, is_write, ctx.session.readonly_reads).error;
}

std::string ScriptClusterRefusal(ServerServices& server, const Session& session, const CommandSpec& spec,
                                 const ArgList& argv, bool allow_cross_slot, int* first_slot) {
  cluster::ClusterManager* cm = server.cluster();
  if (cm == nullptr) return {};
  std::vector<std::string_view> keys;
  AppendKeys(spec, argv, keys);
  const cluster::RouteDecision d =
      cluster::Route(*cm->Current(), keys, (spec.flags & kCmdWrite) != 0, session.readonly_reads);
  if (!d.error.empty()) return std::string(cluster::kScriptErrNonLocalKey);
  if (d.slot < 0 || allow_cross_slot) return {};
  if (*first_slot < 0) {
    *first_slot = d.slot;
    return {};
  }
  return *first_slot == d.slot ? std::string() : std::string(cluster::kScriptErrCrossSlot);
}

std::optional<std::string> BlockedRedirect(const cluster::Topology& t, const BlockedClient& c) {
  if (c.keys().empty()) return std::nullopt;
  const std::vector<std::string_view> keys{c.keys().front()};
  cluster::RouteDecision d = cluster::Route(t, keys, /*is_write=*/true, c.session().readonly_reads);
  if (d.error.empty()) return std::nullopt;
  return std::move(d.error);
}

}  // namespace tideskv
