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

// WATCH key [key ...] (multi.c:470). Records, per key, whether it exists but is logically
// expired now (wk->expired), read before registering: a write between the read and the
// registration commits before anything this client reads afterwards, so nothing is missed.
void Watch(ExecContext& ctx, const ArgList& argv) {
  Session& s = ctx.session;
  if (s.multi.active) return ctx.reply.Error(kErrWatchInMulti);
  if (!s.watch) s.watch = std::make_shared<WatchClient>();
  if (s.watch->dirty()) return ctx.reply.SimpleString("OK");  // Redis: a dirty client registers nothing
  std::vector<char> expired(argv.size(), 0);
  const Status st = ctx.server.engine().Read([&](ReadView& view, int64_t now_ms) {
    for (size_t i = 1; i < argv.size(); ++i) {
      auto obj = LoadObject(view, s.db, argv[i], now_ms);
      if (!obj.ok()) return obj.status();
      expired[i] = obj->state == ObjectState::kExpired ? 1 : 0;
    }
    return Status::Ok();
  });
  if (!st.ok()) return ReplyError(ctx, st);
  for (size_t i = 1; i < argv.size(); ++i)
    ctx.server.watches().Watch(s.watch, s.db, argv[i], expired[i] != 0);
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
