#include "commands/cluster_routing.h"

#include <string_view>
#include <vector>

#include "cluster/cluster_manager.h"
#include "cluster/slot_router.h"
#include "commands/key_extraction.h"
#include "commands/server_services.h"

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

}  // namespace tideskv
