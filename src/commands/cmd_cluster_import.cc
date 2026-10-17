#include <string>
#include <vector>

#include "cluster/cluster_errors.h"
#include "cluster/nodes_parser.h"
#include "commands/cluster_import.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "common/string_util.h"
#include "migrate/migration_service.h"
#include "repl/replication.h"

// Kvrocks parse and texts (cmd_cluster.cc:48-61, cluster.cc:348-404); IMPORTDATA is TidesKV's (D3).
namespace tideskv {
namespace {

constexpr int64_t kImportStart = 0;
constexpr int64_t kImportSuccess = 1;
constexpr int64_t kImportFailed = 2;

Status Err(std::string_view text) { return Status::UserError(std::string(text)); }

Status ParseImport(const ArgList& argv, cluster::SlotRange* range, int64_t* state) {
  std::vector<cluster::SlotRange> ranges;
  if (Status st = MoveInto(cluster::ParseSlotRanges(argv[2]), &ranges); !st.ok()) return st;
  if (ranges.size() != 1) return Err(cluster::kErrSlotRangeInvalid);
  if (!ParseInt64(argv[3], state) || *state < kImportStart || *state > kImportFailed) {
    return Err(cluster::kErrInvalidImportState);
  }
  *range = ranges[0];
  return Status::Ok();
}

// cluster.cc:348-361: a master of the topology, not replicating, that owns no slot of the range.
Status CheckImportable(ExecContext& ctx, const cluster::Topology& t, cluster::SlotRange r) {
  const cluster::ClusterNode* me = t.Myself();
  if (me == nullptr || me->role != cluster::NodeRole::kMaster || ctx.server.replication().IsReplica()) {
    return Err(cluster::kErrSlaveCantImport);
  }
  for (uint32_t s = r.start; s <= r.end; ++s) {
    if (t.OwnerOf(static_cast<uint16_t>(s)) == me) return Err(cluster::kErrImportOwnSlot);
  }
  return Status::Ok();
}

void MarkImportLink(ExecContext& ctx, migrate::MigrationService& m) {
  ctx.session.import_link = true;
  const uint64_t link = ctx.session.id;
  ctx.session.on_close = [&m, link] { m.importer().LinkClosed(link); };  // the service outlives connections
}

}  // namespace

void ClusterImport(ExecContext& ctx, const cluster::Topology& t, const ArgList& argv) {
  migrate::MigrationService* m = ctx.server.migration();
  if (m == nullptr) return ctx.reply.Error(cluster::kErrClusterDisabled);
  cluster::SlotRange range;
  int64_t state = 0;
  if (Status st = ParseImport(argv, &range, &state); !st.ok()) return ReplyError(ctx, st);
  if (Status st = CheckImportable(ctx, t, range); !st.ok()) return ReplyError(ctx, st);
  Status st;
  if (state == kImportStart) {
    st = m->importer().Start(range, ctx.session.id);
    if (st.ok()) MarkImportLink(ctx, *m);
  } else {
    st = m->importer().Finish(range, state == kImportSuccess);
    if (st.ok()) {
      ctx.session.import_link = false;
      ctx.session.on_close = nullptr;
    }
  }
  if (!st.ok()) return ReplyError(ctx, st);
  ctx.reply.SimpleString("OK");
}

void ClusterImportData(ExecContext& ctx, const ArgList& argv) {
  migrate::MigrationService* m = ctx.server.migration();
  if (m == nullptr) return ctx.reply.Error(cluster::kErrClusterDisabled);
  if (!ctx.session.import_link) return ctx.reply.Error(cluster::kErrNotImportLink);
  if (Status st = m->importer().Data(ctx.session.id, argv[2]); !st.ok()) return ReplyError(ctx, st);
  ctx.reply.SimpleString("OK");
}

}  // namespace tideskv
