#include <string>
#include <vector>

#include "cluster/cluster_errors.h"
#include "cluster/cluster_manager.h"
#include "cluster/nodes_parser.h"
#include "commands/all_commands.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "common/string_util.h"

// CLUSTERX (Kvrocks v2.17.0 cmd_cluster.cc CommandClusterX; M7a D8, D11): parsed first, then refused when
// cluster mode is off, as Kvrocks. MIGRATE and FLUSHSLOTS are parsed and refused until M7b.
namespace tideskv {
namespace {

using cluster::ClusterManager;

struct ClusterxCall {
  std::string sub;
  int64_t version = 0;
  bool force = false;
  std::vector<cluster::SlotRange> slots;
};

Status Err(std::string_view text) { return Status::UserError(std::string(text)); }

Status ParseMigrate(const ArgList& argv, ClusterxCall* c) {  // cmd_cluster.cc:155-191
  if (argv.size() < 4 || argv.size() > 6) return Err(cluster::kErrWrongArguments);
  if (Status st = MoveInto(cluster::ParseSlotRanges(argv[2]), &c->slots); !st.ok()) return st;
  if (argv.size() == 4) return Status::Ok();
  const std::string flag = ToLower(argv[4]);
  if (flag == "async") return argv.size() == 6 ? Err(cluster::kErrAsyncTimeout) : Status::Ok();
  if (flag != "sync") return Err(cluster::kErrInvalidSyncFlag);
  if (argv.size() == 6) {
    int64_t timeout = 0;
    if (!ParseInt64(argv[5], &timeout)) return Err(cluster::kErrMigrateTimeout);
    if (timeout < 0) return Err(cluster::kErrMigrateTimeoutNegative);
  }
  return Status::Ok();
}

Status ParseSetNodes(const ArgList& argv, ClusterxCall* c) {  // cmd_cluster.cc:193-211
  if (!ParseInt64(argv[3], &c->version)) return Err(cluster::kErrInvalidVersion);
  if (argv.size() == 4) return Status::Ok();
  if (argv.size() == 5 && EqualsIgnoreCase(argv[4], "force")) {
    c->force = true;
    return Status::Ok();
  }
  return Err(cluster::kErrInvalidSetnodesOptions);
}

Status ParseSetSlot(const ArgList& argv, ClusterxCall* c) {  // cmd_cluster.cc:214-238
  if (Status st = MoveInto(cluster::ParseSlotRanges(argv[2]), &c->slots); !st.ok()) return st;
  if (!EqualsIgnoreCase(argv[3], "node")) return Err(cluster::kErrInvalidSetslotOptions);
  if (argv[4].size() != cluster::kNodeIdLength) return Err(cluster::kErrSetslotNodeId);
  if (!ParseInt64(argv[5], &c->version)) return Err(kErrNotInteger);
  if (c->version < 0) return Err(cluster::kErrInvalidVersion);
  return Status::Ok();
}

Status Parse(const ArgList& argv, ClusterxCall* c) {
  c->sub = ToLower(argv[1]);
  const std::string& sub = c->sub;
  if (argv.size() == 2 && (sub == "version" || sub == "myid")) return Status::Ok();
  if (sub == "setnodeid" && argv.size() == 3 && argv[2].size() == cluster::kNodeIdLength) return Status::Ok();
  if (sub == "migrate") return ParseMigrate(argv, c);
  if (sub == "setnodes" && argv.size() >= 4) return ParseSetNodes(argv, c);
  if (sub == "setslot" && argv.size() == 6) return ParseSetSlot(argv, c);
  if (sub == "flushslots") {
    if (argv.size() != 3) return Err(cluster::kErrWrongArguments);
    return MoveInto(cluster::ParseSlotRanges(argv[2]), &c->slots);
  }
  return Err(cluster::kErrClusterxUsage);
}

void Run(ExecContext& ctx, ClusterManager& cm, const ClusterxCall& c, const ArgList& argv) {
  if (c.sub == "version") return ctx.reply.Bulk(std::to_string(cm.Current()->version));
  if (c.sub == "myid") return ctx.reply.Bulk(cm.Current()->myid);
  if (c.sub == "migrate" || c.sub == "flushslots") return ctx.reply.Error(cluster::kErrMigrateNotSupported);
  Status st = Status::Ok();
  if (c.sub == "setnodes") {
    st = cm.SetNodes(argv[2], c.version, c.force);
  } else if (c.sub == "setnodeid") {
    st = cm.SetNodeId(argv[2]);
  } else {
    st = cm.SetSlot(c.slots, argv[4], c.version);
  }
  if (!st.ok()) return ReplyError(ctx, st);
  ctx.reply.SimpleString("OK");
}

void Clusterx(ExecContext& ctx, const ArgList& argv) {
  ClusterxCall call;
  if (Status st = Parse(argv, &call); !st.ok()) return ReplyError(ctx, st);
  ClusterManager* cm = ctx.server.cluster();
  if (cm == nullptr) return ctx.reply.Error(cluster::kErrClusterNotEnabled);
  Run(ctx, *cm, call, argv);
}

}  // namespace

void RegisterClusterxCommand(CommandRegistry& r) {
  // no_multi + noscript: a role change never runs under the Engine gate (D8); loading + stale: a controller
  // may repoint a replica that is full-syncing (M6b D7).
  r.Register(
      {"clusterx", -2, kCmdAdmin | kCmdNoScript | kCmdNoMulti | kCmdLoading | kCmdStale, 0, 0, 0, &Clusterx});
}

}  // namespace tideskv
