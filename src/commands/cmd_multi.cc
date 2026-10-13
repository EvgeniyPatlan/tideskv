#include <functional>
#include <memory>
#include <vector>

#include "commands/all_commands.h"
#include "commands/multi_state.h"
#include "commands/registry.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "exec/engine.h"
#include "resp/reply_builder.h"
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

void Multi(ExecContext& ctx, const ArgList&) {
  if (ctx.session.multi.active) return ctx.reply.Error(kErrMultiNested);
  ctx.session.multi.active = true;
  ctx.reply.SimpleString("OK");
}

void Discard(ExecContext& ctx, const ArgList&) {
  if (!ctx.session.multi.active) return ctx.reply.Error(kErrDiscardNoMulti);
  DiscardTransaction(ctx);
  ctx.reply.SimpleString("OK");
}

// EXEC's CAS check, run while the batch holds the gate exclusively, so every committed write has
// already reported its keys (Review Focus #1). Task 6 adds the expiry half.
Result<bool> WatchFailed(ExecContext& ctx) {
  const auto& w = ctx.session.watch;
  return w != nullptr && w->dirty();
}

// Runs the queue in one batch (G1, G3). Replies go to a scratch builder that is sent only if the
// batch commits; a failed commit answers one error and applies nothing. Blocking commands see
// block == nullptr; keys they make ready are served by the caller after EXEC returns.
void RunQueued(ExecContext& ctx, const std::vector<ArgList>& queued, std::unique_ptr<Engine::Batch> batch) {
  resp::ReplyBuilder replies(ctx.reply.protocol());
  std::vector<std::function<void()>> after;
  ExecContext inner{ctx.session, replies, ctx.server, nullptr, ctx.ready, &after};
  const CommandRegistry& registry = ctx.server.registry();
  for (const ArgList& argv : queued) registry.Find(argv[0])->handler(inner, argv);  // validated when queued
  const Status st = batch->Commit();
  batch.reset();  // releases the gate before the deferred actions run
  if (!st.ok()) return ReplyError(ctx, st);
  ctx.reply.ArrayHeader(queued.size());
  ctx.reply.Raw(replies.buffer());
  ctx.reply.SetProtocol(replies.protocol());  // HELLO inside the transaction switched it
  for (const auto& action : after) action();
}

void Exec(ExecContext& ctx, const ArgList&) {
  MultiState& m = ctx.session.multi;
  if (!m.active) return ctx.reply.Error(kErrExecNoMulti);
  if (m.dirty_exec) {
    DiscardTransaction(ctx);
    return ctx.reply.Error(kErrExecAbort);
  }
  auto batch = ctx.server.engine().BeginBatch();
  if (!batch.ok()) {
    DiscardTransaction(ctx);
    return ReplyError(ctx, batch.status());
  }
  const Result<bool> failed = WatchFailed(ctx);
  if (!failed.ok() || *failed) {
    batch->reset();  // roll back and release the gate first
    DiscardTransaction(ctx);
    return failed.ok() ? ctx.reply.NullArray() : ReplyError(ctx, failed.status());
  }
  const std::vector<ArgList> queued = std::move(m.queued);
  DiscardTransaction(ctx);  // leaves MULTI and unwatches before the queue runs (multi.c:187)
  RunQueued(ctx, queued, std::move(batch).value());
}

}  // namespace

void RegisterMultiCommands(CommandRegistry& r) {
  r.Register({"watch", -2, kCmdNoScript | kCmdFast, 1, -1, 1, &Watch});
  r.Register({"multi", 1, kCmdNoScript | kCmdFast, 0, 0, 0, &Multi});
  r.Register({"exec", 1, kCmdNoScript, 0, 0, 0, &Exec});
  r.Register({"discard", 1, kCmdNoScript | kCmdFast, 0, 0, 0, &Discard});
  r.Register({"unwatch", 1, kCmdNoScript | kCmdFast, 0, 0, 0, &Unwatch});
}

}  // namespace tideskv
