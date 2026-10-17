#include "commands/cluster_routing.h"

#include <string_view>
#include <vector>

#include "cluster/cluster_errors.h"
#include "cluster/cluster_manager.h"
#include "cluster/slot_router.h"
#include "cluster/slot_states.h"
#include "commands/key_extraction.h"
#include "commands/server_services.h"
#include "common/crc16.h"
#include "exec/engine.h"
#include "server/blocking.h"
#include "types/errors.h"

namespace tideskv {
namespace {

static_assert(cluster::kErrTryAgainFenced == kErrTryAgain, "the TRYAGAIN text lives in two headers");

void AppendKeys(const CommandSpec& spec, const ArgList& argv, std::vector<std::string_view>& keys) {
  for (size_t i : KeyIndexes(spec, argv)) keys.emplace_back(argv[i]);
}

}  // namespace

std::vector<std::string_view> CallKeys(ExecContext& ctx, const CommandSpec& spec, const ArgList& argv) {
  std::vector<std::string_view> keys;
  if (spec.name != "exec") {
    AppendKeys(spec, argv, keys);
    return keys;
  }
  for (const ArgList& q : ctx.session.multi.queued) {
    if (const CommandSpec* s = ctx.server.registry().Find(q[0]); s != nullptr) AppendKeys(*s, q, keys);
  }
  return keys;
}

std::string ClusterRefusal(ExecContext& ctx, const CommandSpec& spec, const ArgList& argv, bool is_write) {
  cluster::ClusterManager* cm = ctx.server.cluster();
  if (cm == nullptr) return {};
  if (spec.name == "exec" && !ctx.session.multi.active)
    return {};  // EXEC answers "EXEC without MULTI" itself
  const std::vector<std::string_view> keys = CallKeys(ctx, spec, argv);
  if (keys.empty()) return {};
  return cluster::Route(*cm->Current(), *cm->States(), keys, is_write, ctx.session.readonly_reads,
                        ctx.session.asking)
      .error;
}

std::string FencedCallRefusal(ExecContext& ctx, const std::vector<std::string_view>& keys, bool writes) {
  if (ctx.server.cluster() == nullptr || !writes || keys.empty()) return {};
  return ctx.server.engine().SlotFenced(KeySlot(keys.front())) ? std::string(kErrTryAgain) : std::string();
}

std::string ScriptClusterRefusal(ServerServices& server, const Session& session, const CommandSpec& spec,
                                 const ArgList& argv, bool allow_cross_slot, int* first_slot) {
  cluster::ClusterManager* cm = server.cluster();
  if (cm == nullptr) return {};
  std::vector<std::string_view> keys;
  AppendKeys(spec, argv, keys);
  const cluster::RouteDecision d =
      cluster::Route(*cm->Current(), *cm->States(), keys, (spec.flags & kCmdWrite) != 0,
                     session.readonly_reads, session.asking);
  if (d.error.starts_with("TRYAGAIN")) return d.error;  // M7b D4: a fenced slot is local
  if (!d.error.empty()) return std::string(cluster::kScriptErrNonLocalKey);
  if (d.slot < 0 || allow_cross_slot) return {};
  if (*first_slot < 0) {
    *first_slot = d.slot;
    return {};
  }
  return *first_slot == d.slot ? std::string() : std::string(cluster::kScriptErrCrossSlot);
}

std::optional<std::string> BlockedRedirect(const cluster::Topology& t, const cluster::SlotStates& s,
                                           const BlockedClient& c) {
  if (c.keys().empty()) return std::nullopt;
  const std::vector<std::string_view> keys{c.keys().front()};
  cluster::RouteDecision d =
      cluster::Route(t, s, keys, /*is_write=*/true, c.session().readonly_reads, c.session().asking);
  if (d.error.empty() || d.error.starts_with("TRYAGAIN")) return std::nullopt;
  return std::move(d.error);
}

std::optional<std::string> BlockedRedirect(const cluster::Topology& t, const BlockedClient& c) {
  static const cluster::SlotStates kNone;
  return BlockedRedirect(t, kNone, c);
}

}  // namespace tideskv
