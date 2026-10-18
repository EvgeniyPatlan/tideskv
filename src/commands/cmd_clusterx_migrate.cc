#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "cluster/cluster_errors.h"
#include "commands/clusterx_migrate.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "migrate/migration_service.h"
#include "server/blocking.h"

namespace tideskv {
namespace {

// D12: the SYNC reply's rendezvous; the job may end before the connection has parked (an empty slot).
struct SyncWait {
  std::mutex mu;
  std::optional<std::string> reply;       // the encoded outcome, when it came first
  std::shared_ptr<BlockedClient> client;  // the parked call, when it came first
};

std::string OutcomeReply(const Status& st) { return st.ok() ? "+OK\r\n" : "-" + st.message() + "\r\n"; }

migrate::MigrateDone Deliverer(const std::shared_ptr<SyncWait>& wait) {
  return [wait](const Status& outcome) {
    std::shared_ptr<BlockedClient> client;
    {
      std::lock_guard lock(wait->mu);
      if (!wait->client) {
        wait->reply = OutcomeReply(outcome);
        return;
      }
      client = wait->client;
    }
    // False when the wait already timed out: the job went on without its caller (KM §2.3).
    client->Unblock(OutcomeReply(outcome));
  };
}

// Parks the call like WAIT (a keyless BlockedClient, M6b D9): timeout 0 waits forever; the timeout reply is
// nil and the job continues (Kvrocks sync_migrate_context.cc:53-64).
void Park(ExecContext& ctx, const std::shared_ptr<SyncWait>& wait, int64_t timeout_s) {
  BlockRequest req;
  req.timeout_ms = std::min<int64_t>(timeout_s, std::numeric_limits<int64_t>::max() / 1000) * 1000;
  const int protocol = ctx.session.protocol;
  req.timeout_reply = [protocol] { return std::string(protocol == 3 ? "_\r\n" : "$-1\r\n"); };
  ctx.block->Block(req);
  const std::shared_ptr<BlockedClient> client = ctx.session.blocked;
  if (!client) return;
  std::optional<std::string> early;
  {
    std::lock_guard lock(wait->mu);
    early = wait->reply;
    if (!early) wait->client = client;
  }
  if (early) client->Unblock(*early);
}

}  // namespace

void RunMigrate(ExecContext& ctx, const std::vector<cluster::SlotRange>& slots, const std::string& dst_id,
                bool sync, int64_t timeout_s) {
  migrate::MigrationService* m = ctx.server.migration();
  if (m == nullptr) return ctx.reply.Error(cluster::kErrClusterNotEnabled);
  if (!sync || ctx.block == nullptr) {  // CLUSTERX never runs in MULTI or Lua, so there is always a handler
    if (Status st = m->Migrate(slots, dst_id, nullptr); !st.ok()) return ReplyError(ctx, st);
    return ctx.reply.SimpleString("OK");
  }
  auto wait = std::make_shared<SyncWait>();
  if (Status st = m->Migrate(slots, dst_id, Deliverer(wait)); !st.ok()) return ReplyError(ctx, st);
  Park(ctx, wait, timeout_s);
}

// Parity: the erase does not invalidate WATCH on the flushed keys, as Kvrocks ClearKeysOfSlotRange does not.
void RunFlushSlots(ExecContext& ctx, const std::vector<cluster::SlotRange>& slots) {
  migrate::MigrationService* m = ctx.server.migration();
  if (m == nullptr) return ctx.reply.Error(cluster::kErrClusterNotEnabled);
  if (Status st = m->FlushSlots(slots); !st.ok()) return ReplyError(ctx, st);
  ctx.reply.SimpleString("OK");
}

}  // namespace tideskv
