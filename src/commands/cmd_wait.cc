#include <cstdint>
#include <limits>
#include <string>

#include "commands/all_commands.h"
#include "commands/server_services.h"
#include "common/string_util.h"
#include "exec/engine.h"
#include "repl/replication.h"
#include "types/errors.h"

namespace tideskv {
namespace {

// getTimeoutFromObjectOrReply(UNIT_MILLISECONDS) (timeout.c:165): *ms = 0 means forever. False after
// replying the error.
bool ParseWaitTimeout(ExecContext& ctx, const std::string& arg, int64_t* ms) {
  if (!ParseInt64(arg, ms)) {
    ctx.reply.Error(kErrTimeoutNotInteger);
    return false;
  }
  if (*ms < 0) {
    ctx.reply.Error(kErrTimeoutNegative);
    return false;
  }
  if (*ms > 0 && *ms > std::numeric_limits<int64_t>::max() - ctx.server.engine().NowMs()) {
    ctx.reply.Error(kErrTimeoutOutOfRange);  // the absolute deadline would overflow
    return false;
  }
  return true;
}

// WAIT numreplicas timeout (waitCommand, replication.c:3537; M6b D9): the number of online replicas that
// acknowledged this client's last write, once numreplicas of them did or the timeout passed. Inside MULTI
// or a script (no BlockHandler) it answers the current count at once.
void Wait(ExecContext& ctx, const ArgList& argv) {
  Replication& repl = ctx.server.replication();
  if (repl.IsReplica()) return ctx.reply.Error(kErrWaitOnReplica);  // before parsing
  int64_t numreplicas = 0;
  if (!ParseInt64(argv[1], &numreplicas)) return ctx.reply.Error(kErrNotInteger);
  int64_t timeout_ms = 0;
  if (!ParseWaitTimeout(ctx, argv[2], &timeout_ms)) return;
  const uint64_t woff = ctx.session.woff;
  const int64_t acked = repl.CountAcked(woff);
  if (acked >= numreplicas || ctx.block == nullptr) return ctx.reply.Integer(acked);
  BlockRequest req;
  req.timeout_ms = timeout_ms;
  // Built when the wait ends by timeout or CLIENT UNBLOCK TIMEOUT: the count at that moment (Redis
  // replyToBlockedClientTimedOut). The server outlives every connection, so `repl` does too.
  req.timeout_reply = [&repl, woff] { return ":" + std::to_string(repl.CountAcked(woff)) + "\r\n"; };
  ctx.block->Block(req);
  if (ctx.session.blocked) repl.AddWaiter(ctx.session.blocked, woff, numreplicas);
}

}  // namespace

void RegisterWaitCommand(CommandRegistry& r) { r.Register({"wait", 3, 0, 0, 0, 0, &Wait}); }

}  // namespace tideskv
