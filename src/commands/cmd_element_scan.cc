#include <optional>
#include <string>

#include "commands/all_commands.h"
#include "commands/cmd_util.h"
#include "commands/reply_util.h"
#include "commands/scan_args.h"
#include "commands/server_services.h"
#include "types/collection.h"
#include "types/cursor_store.h"
#include "types/scan.h"

namespace tideskv {
namespace {

using encoding::ValueType;

// HSCAN/SSCAN key cursor [MATCH pattern] [COUNT count]. Redis 7.2 order: the cursor, then the
// key lookup (missing: cursor 0 and no elements; wrong type: WRONGTYPE), then the options.
void ElementScanCommand(ExecContext& ctx, const ArgList& argv, ValueType type) {
  uint64_t cursor = 0;
  if (!ParseScanCursor(argv[2], &cursor)) return ctx.reply.Error(kErrInvalidCursor);
  ScanOptions opts;
  const Status opts_status = ParseScanOptions(argv, 3, /*allow_type=*/false, &opts);
  const std::optional<ScanPosition> saved =
      cursor == 0 ? std::nullopt : ctx.server.cursors().Load(cursor, CursorKind::kElements);
  ElementPage page;
  const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) -> Status {
    page = ElementPage{};
    auto c = LoadCollection(view, ctx.session.db, argv[1], type, now_ms);
    if (!c.ok()) return c.status();
    if (!c->exists()) return Status::Ok();
    if (!opts_status.ok()) return opts_status;
    const std::string prefix = c->Prefix();
    // A position saved in another collection (or an older incarnation of this key: other oid) restarts.
    const ScanPosition position = saved && saved->key.starts_with(prefix) ? *saved : ScanPosition{};
    return ScanElements(view, prefix, position, opts.count, opts.filter.pattern, type == ValueType::kHash,
                        &page);
  });
  if (!ok) return;
  const uint64_t next = page.next_position.empty()
                            ? 0
                            : ctx.server.cursors().Save(std::move(page.next_position), CursorKind::kElements);
  ctx.reply.ArrayHeader(2);
  ctx.reply.Bulk(std::to_string(next));
  ctx.reply.ArrayHeader(page.items.size());
  for (const std::string& s : page.items) ctx.reply.Bulk(s);
}

void HScan(ExecContext& ctx, const ArgList& argv) { ElementScanCommand(ctx, argv, ValueType::kHash); }
void SScan(ExecContext& ctx, const ArgList& argv) { ElementScanCommand(ctx, argv, ValueType::kSet); }

}  // namespace

void RegisterElementScanCommands(CommandRegistry& r) {
  r.Register({"hscan", -3, kCmdReadOnly, 1, 1, 1, &HScan});
  r.Register({"sscan", -3, kCmdReadOnly, 1, 1, 1, &SScan});
}

}  // namespace tideskv
