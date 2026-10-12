#include <string>
#include <vector>

#include "commands/all_commands.h"
#include "commands/command_rng.h"
#include "commands/reply_util.h"
#include "commands/scan_args.h"
#include "commands/server_services.h"
#include "common/string_util.h"
#include "encoding/key_codec.h"
#include "exec/engine.h"
#include "types/cursor_store.h"
#include "types/scan.h"

namespace tideskv {
namespace {

// Resolves the cursor to a position inside the current db (D9: unknown or foreign = restart).
Status ResolveCursor(ExecContext& ctx, const std::string& arg, ScanPosition* position) {
  uint64_t cursor = 0;
  if (!ParseScanCursor(arg, &cursor)) return Status::UserError(std::string(kErrInvalidCursor));
  *position = ScanPosition{};
  if (cursor == 0) return Status::Ok();
  std::optional<ScanPosition> saved = ctx.server.cursors().Load(cursor);
  if (saved && saved->key.starts_with(encoding::DbPrefix(ctx.session.db))) *position = std::move(*saved);
  return Status::Ok();
}

void Scan(ExecContext& ctx, const ArgList& argv) {
  ScanPosition position;  // where to resume; empty key = start
  ScanOptions opts;
  if (Status st = ResolveCursor(ctx, argv[1], &position); !st.ok()) return ReplyError(ctx, st);
  if (Status st = ParseScanOptions(argv, 2, /*allow_type=*/true, &opts); !st.ok()) return ReplyError(ctx, st);
  Engine& e = ctx.server.engine();
  ScanPage page;
  const Status st = e.Snapshot([&](ReadView& v, int64_t now_ms) {
    return ScanKeys(v, ctx.session.db, position, opts.count, opts.filter, now_ms, &page);
  });
  if (!st.ok()) return ReplyError(ctx, st);
  const uint64_t next =
      page.next_position.empty() ? 0 : ctx.server.cursors().Save(std::move(page.next_position));
  ctx.reply.ArrayHeader(2);
  ctx.reply.Bulk(std::to_string(next));
  ctx.reply.ArrayHeader(page.keys.size());
  for (const std::string& k : page.keys) ctx.reply.Bulk(k);
}

void Keys(ExecContext& ctx, const ArgList& argv) {
  Engine& e = ctx.server.engine();
  std::vector<std::string> keys;
  const Status st = e.Snapshot(
      [&](ReadView& v, int64_t now_ms) { return MatchingKeys(v, ctx.session.db, argv[1], now_ms, &keys); });
  if (!st.ok()) return ReplyError(ctx, st);
  ctx.reply.ArrayHeader(keys.size());
  for (const std::string& k : keys) ctx.reply.Bulk(k);
}

void RandomKeyCmd(ExecContext& ctx, const ArgList&) {
  Engine& e = ctx.server.engine();
  std::optional<std::string> key;
  const Status st = e.Snapshot([&](ReadView& v, int64_t now_ms) {
    return MoveInto(RandomKey(v, ctx.session.db, now_ms, CommandRng()()), &key);
  });
  if (!st.ok()) return ReplyError(ctx, st);
  key ? ctx.reply.Bulk(*key) : ctx.reply.Null();
}

}  // namespace

void RegisterScanCommands(CommandRegistry& r) {
  r.Register({"scan", -2, kCmdReadOnly, 0, 0, 0, &Scan});
  r.Register({"keys", 2, kCmdReadOnly, 0, 0, 0, &Keys});
  r.Register({"randomkey", 1, kCmdReadOnly, 0, 0, 0, &RandomKeyCmd});
}

}  // namespace tideskv
