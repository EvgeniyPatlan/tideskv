#include <memory>
#include <vector>

#include "commands/all_commands.h"
#include "commands/multi_state.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "exec/engine.h"
#include "server/watch.h"
#include "types/errors.h"
#include "types/object_store.h"

namespace tideskv {
namespace {

// WATCH key [key ...] (multi.c:470). Registers every key first (expired=false), then reads each
// key's state and flags those that exist but are logically expired now (wk->expired). A write
// landing between registration and the flag dirties the client (a spurious abort is safe); the
// flag is never set from a read older than the registration, so it cannot swallow a later change.
void Watch(ExecContext& ctx, const ArgList& argv) {
  Session& s = ctx.session;
  if (s.multi.active) return ctx.reply.Error(kErrWatchInMulti);
  if (!s.watch) s.watch = std::make_shared<WatchClient>();
  if (s.watch->dirty()) return ctx.reply.SimpleString("OK");  // Redis: a dirty client registers nothing
  std::vector<char> fresh(argv.size(), 0);
  for (size_t i = 1; i < argv.size(); ++i)
    fresh[i] = ctx.server.watches().Watch(s.watch, s.db, argv[i], false);
  std::vector<char> expired(argv.size(), 0);
  const Status st = ctx.server.engine().Read([&](ReadView& view, int64_t now_ms) {
    for (size_t i = 1; i < argv.size(); ++i) {
      if (!fresh[i]) continue;
      auto obj = LoadObject(view, s.db, argv[i], now_ms);
      if (!obj.ok()) return obj.status();
      expired[i] = obj->state == ObjectState::kExpired ? 1 : 0;
    }
    return Status::Ok();
  });
  if (!st.ok()) return ReplyError(ctx, st);
  for (size_t i = 1; i < argv.size(); ++i) {
    if (expired[i]) ctx.server.watches().MarkExpiredAtWatch(*s.watch, s.db, argv[i]);
  }
  ctx.reply.SimpleString("OK");
}

// UNWATCH (multi.c:487): clears DIRTY_CAS too, not DIRTY_EXEC.
void Unwatch(ExecContext& ctx, const ArgList&) {
  UnwatchClient(ctx);
  ctx.reply.SimpleString("OK");
}

}  // namespace

void RegisterMultiCommands(CommandRegistry& r) {
  r.Register({"watch", -2, kCmdNoScript | kCmdFast, 1, -1, 1, &Watch});
  r.Register({"unwatch", 1, kCmdNoScript | kCmdFast, 0, 0, 0, &Unwatch});
}

}  // namespace tideskv
