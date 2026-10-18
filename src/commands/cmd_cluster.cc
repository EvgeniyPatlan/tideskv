#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cluster/cluster_errors.h"
#include "cluster/cluster_manager.h"
#include "cluster/cluster_views.h"
#include "commands/all_commands.h"
#include "commands/cluster_import.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "common/crc16.h"
#include "common/string_util.h"
#include "exec/engine.h"
#include "migrate/migration_service.h"
#include "repl/replication.h"
#include "types/slot_keys.h"

// CLUSTER (M7a D11, D12): Redis 7.2 subcommand names and shapes, Kvrocks texts where the controller parses
// them.
namespace tideskv {
namespace {

using cluster::ClusterManager;
using cluster::Topology;
using SubFn = void (*)(ExecContext&, ClusterManager&, const Topology&, const ArgList&);

struct Sub {
  size_t min_args;
  size_t max_args;
  bool needs_topology;  // CLUSTERDOWN The cluster is not initialized before the first SETNODES
  SubFn run;
};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void Info(ExecContext& ctx, ClusterManager& cm, const Topology& t, const ArgList&) {
  ctx.reply.Verbatim(cluster::ClusterInfoText(t, *cm.States(), ctx.server.replication().IsReplica()));
}

void Nodes(ExecContext& ctx, ClusterManager&, const Topology& t, const ArgList&) {
  ctx.reply.Verbatim(cluster::ClusterNodesText(t, NowMs()));
}

void Slots(ExecContext& ctx, ClusterManager&, const Topology& t, const ArgList&) {
  const auto runs = cluster::SlotRuns(t);
  ctx.reply.ArrayHeader(runs.size());
  for (const auto& run : runs) {
    ctx.reply.ArrayHeader(2 + run.nodes.size());
    ctx.reply.Integer(run.start);
    ctx.reply.Integer(run.end);
    for (const cluster::ClusterNode* n : run.nodes) {  // Kvrocks: [host, port, id], no metadata map (D12)
      ctx.reply.ArrayHeader(3);
      ctx.reply.Bulk(n->host);
      ctx.reply.Integer(n->port);
      ctx.reply.Bulk(n->id);
    }
  }
}

void ShardNode(resp::ReplyBuilder& r, const cluster::ClusterNode& n, bool master, int64_t offset) {
  r.MapHeader(7);
  r.Bulk("id");
  r.Bulk(n.id);
  r.Bulk("port");
  r.Integer(n.port);
  r.Bulk("ip");
  r.Bulk(n.host);
  r.Bulk("endpoint");
  r.Bulk(n.host);
  r.Bulk("role");
  r.Bulk(master ? "master" : "replica");
  r.Bulk("replication-offset");
  r.Integer(offset);
  r.Bulk("health");
  r.Bulk(n.failed ? "fail" : "online");
}

void Shards(ExecContext& ctx, ClusterManager&, const Topology& t, const ArgList&) {
  const auto shards = cluster::Shards(t);
  const auto mine = static_cast<int64_t>(ctx.server.replication().Role().offset);
  const auto offset = [&](const cluster::ClusterNode* n) { return n == t.Myself() ? mine : int64_t{0}; };
  ctx.reply.ArrayHeader(shards.size());
  for (const auto& s : shards) {
    ctx.reply.MapHeader(2);
    ctx.reply.Bulk("slots");
    ctx.reply.ArrayHeader(2 * s.slots.size());
    for (const auto& r : s.slots) {
      ctx.reply.Integer(r.start);
      ctx.reply.Integer(r.end);
    }
    ctx.reply.Bulk("nodes");
    ctx.reply.ArrayHeader(1 + s.replicas.size());
    ShardNode(ctx.reply, *s.master, true, offset(s.master));
    for (const cluster::ClusterNode* n : s.replicas) ShardNode(ctx.reply, *n, false, offset(n));
  }
}

void MyId(ExecContext& ctx, ClusterManager&, const Topology& t, const ArgList&) { ctx.reply.Bulk(t.myid); }

void KeySlotCmd(ExecContext& ctx, ClusterManager&, const Topology&, const ArgList& argv) {
  ctx.reply.Integer(KeySlot(argv[2]));
}

void CountKeys(ExecContext& ctx, ClusterManager&, const Topology&, const ArgList& argv) {
  int64_t slot = 0;
  if (!ParseInt64(argv[2], &slot)) return ctx.reply.Error(kErrNotInteger);
  if (slot < 0 || slot >= kSlotCount) return ctx.reply.Error(cluster::kErrInvalidSlot);
  uint64_t n = 0;
  const Status st = ctx.server.engine().Read(
      [&](ReadView& v, int64_t) { return MoveInto(CountKeysInSlot(v, static_cast<uint16_t>(slot)), &n); });
  if (!st.ok()) return ReplyError(ctx, st);
  ctx.reply.Integer(static_cast<int64_t>(n));
}

void GetKeys(ExecContext& ctx, ClusterManager&, const Topology&, const ArgList& argv) {
  int64_t slot = 0;
  int64_t count = 0;
  if (!ParseInt64(argv[2], &slot) || !ParseInt64(argv[3], &count)) return ctx.reply.Error(kErrNotInteger);
  if (slot < 0 || slot >= kSlotCount || count < 0) return ctx.reply.Error(cluster::kErrInvalidSlotOrCount);
  std::vector<std::string> keys;
  const Status st = ctx.server.engine().Read([&](ReadView& v, int64_t) {
    return MoveInto(KeysInSlot(v, static_cast<uint16_t>(slot), static_cast<uint64_t>(count)), &keys);
  });
  if (!st.ok()) return ReplyError(ctx, st);
  ctx.reply.ArrayHeader(keys.size());
  for (const std::string& k : keys) ctx.reply.Bulk(k);
}

void Replicas(ExecContext& ctx, ClusterManager&, const Topology& t, const ArgList& argv) {
  auto text = cluster::ClusterReplicasText(t, argv[2], NowMs());
  if (!text.ok()) return ReplyError(ctx, text.status());
  ctx.reply.Verbatim(*text);
}

// D20: the emptiness check and ClusterManager::Reset (whose role change takes no gate) run with every slot
// held fenced (MigrationService::ResetCluster), so no write lands between them (closes the M7a Task 10
// ruling).
Status ResetIfEmpty(ExecContext& ctx, ClusterManager& cm) {
  bool empty = false;
  const Status st =
      ctx.server.engine().Read([&](ReadView& v, int64_t) { return MoveInto(KeyspaceEmpty(v), &empty); });
  return st.ok() ? cm.Reset(empty) : st;
}

void Reset(ExecContext& ctx, ClusterManager& cm, const Topology&, const ArgList& argv) {
  if (argv.size() == 3 && !EqualsIgnoreCase(argv[2], "hard") && !EqualsIgnoreCase(argv[2], "soft")) {
    return ctx.reply.Error(kErrSyntax);
  }
  const auto reset = [&ctx, &cm] { return ResetIfEmpty(ctx, cm); };
  migrate::MigrationService* m = ctx.server.migration();
  Status st = Status::Ok();
  if (m != nullptr) {
    st = m->ResetCluster(reset);
  } else {  // no migration service: nothing else moves the fence
    const Engine::FenceHold hold(ctx.server.engine());
    st = reset();
  }
  if (!st.ok()) return ReplyError(ctx, st);
  ctx.reply.SimpleString("OK");
}

void Import(ExecContext& ctx, ClusterManager&, const Topology& t, const ArgList& argv) {
  ClusterImport(ctx, t, argv);
}

void ImportData(ExecContext& ctx, ClusterManager&, const Topology&, const ArgList& argv) {
  ClusterImportData(ctx, argv);
}

void Help(ExecContext& ctx, ClusterManager&, const Topology&, const ArgList&) {
  static constexpr std::string_view kLines[] = {
      "CLUSTER <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
      "COUNTKEYSINSLOT <slot>",
      "    Return the number of keys in <slot>.",
      "GETKEYSINSLOT <slot> <count>",
      "    Return key names stored by current node in a slot.",
      "INFO",
      "    Return information about the cluster.",
      "KEYSLOT <key>",
      "    Return the hash slot for <key>.",
      "MYID",
      "    Return the node id.",
      "NODES",
      "    Return cluster configuration seen by node. Output format:",
      "    <id> <ip:port@cport> <flags> <master> <pings> <pongs> <epoch> <link> <slot> ...",
      "REPLICAS <node-id>",
      "    Return <node-id> replicas.",
      "RESET [HARD|SOFT]",
      "    Forget the cluster topology (the keyspace must be empty).",
      "SHARDS",
      "    Return information about slot range mappings and the nodes associated with them.",
      "SLOTS",
      "    Return information about slots range mappings. Each range is made of:",
      "    start, end, master and replicas IP addresses, ports and ids",
      "HELP",
      "    Print this help."};
  ctx.reply.ArrayHeader(std::size(kLines));
  for (std::string_view line : kLines) ctx.reply.SimpleString(line);
}

const std::map<std::string, Sub, std::less<>>& Subcommands() {
  static const std::map<std::string, Sub, std::less<>> kSubs = {
      {"countkeysinslot", {3, 3, false, &CountKeys}},
      {"getkeysinslot", {4, 4, false, &GetKeys}},
      {"help", {2, 2, false, &Help}},
      {"import", {4, 4, false, &Import}},  // needs no topology: Kvrocks's "Slave can't import slot" then
      {"importdata", {3, 3, false, &ImportData}},
      {"info", {2, 2, true, &Info}},
      {"keyslot", {3, 3, false, &KeySlotCmd}},
      {"myid", {2, 2, false, &MyId}},
      {"nodes", {2, 2, true, &Nodes}},
      {"replicas", {3, 3, true, &Replicas}},
      {"reset", {2, 3, false, &Reset}},
      {"shards", {2, 2, true, &Shards}},
      {"slots", {2, 2, true, &Slots}}};
  return kSubs;
}

void Cluster(ExecContext& ctx, const ArgList& argv) {
  ClusterManager* cm = ctx.server.cluster();
  if (cm == nullptr) return ctx.reply.Error(cluster::kErrClusterDisabled);
  const auto& subs = Subcommands();
  const auto it = subs.find(ToLower(argv[1]));
  if (it == subs.end() || argv.size() < it->second.min_args || argv.size() > it->second.max_args) {
    return ctx.reply.Error(SubcommandSyntaxError(argv[1], "CLUSTER"));  // D18: names CLUSTER HELP
  }
  const std::shared_ptr<const Topology> t = cm->Current();
  if (it->second.needs_topology && !t->initialized())
    return ctx.reply.Error(cluster::kErrClusterNotInitialized);
  it->second.run(ctx, *cm, *t, argv);
}

// READONLY / READWRITE / ASKING (cluster.c:7282-7311): connection flags read by the router (D13).
void ReadOnlyCmd(ExecContext& ctx, const ArgList&) {
  if (ctx.server.cluster() == nullptr) return ctx.reply.Error(cluster::kErrClusterDisabled);
  ctx.session.readonly_reads = true;
  ctx.reply.SimpleString("OK");
}

void ReadWriteCmd(ExecContext& ctx, const ArgList&) {
  if (ctx.server.cluster() == nullptr) return ctx.reply.Error(cluster::kErrClusterDisabled);
  ctx.session.readonly_reads = false;
  ctx.reply.SimpleString("OK");
}

void AskingCmd(ExecContext& ctx, const ArgList&) {
  if (ctx.server.cluster() == nullptr) return ctx.reply.Error(cluster::kErrClusterDisabled);
  ctx.session.asking = true;
  ctx.reply.SimpleString("OK");
}

}  // namespace

void RegisterClusterCommands(CommandRegistry& r) {
  // loading + stale: a full-syncing replica answers the controller's probe (D12); no_multi + noscript: RESET
  // changes the role (D8).
  r.Register({"cluster", -2, kCmdNoScript | kCmdNoMulti | kCmdLoading | kCmdStale, 0, 0, 0, &Cluster});
  // Redis 7.2: readonly/readwrite "fast loading stale", asking "fast".
  r.Register({"readonly", 1, kCmdFast | kCmdLoading | kCmdStale, 0, 0, 0, &ReadOnlyCmd});
  r.Register({"readwrite", 1, kCmdFast | kCmdLoading | kCmdStale, 0, 0, 0, &ReadWriteCmd});
  r.Register({"asking", 1, kCmdFast, 0, 0, 0, &AskingCmd});
}

}  // namespace tideskv
