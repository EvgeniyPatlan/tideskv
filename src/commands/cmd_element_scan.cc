#include <optional>
#include <string>

#include "commands/all_commands.h"
#include "commands/cmd_util.h"
#include "commands/reply_util.h"
#include "commands/scan_args.h"
#include "commands/server_services.h"
#include "common/glob.h"
#include "common/score_format.h"
#include "types/collection.h"
#include "types/cursor_store.h"
#include "types/scan.h"
#include "types/zset_pop.h"
#include "types/zset_type.h"

namespace tideskv {
namespace {

using encoding::ValueType;

void ReplyElementPage(ExecContext& ctx, ElementPage& page) {
  const uint64_t next = page.next_position.empty()
                            ? 0
                            : ctx.server.cursors().Save(std::move(page.next_position), CursorKind::kElements);
  ctx.reply.ArrayHeader(2);
  ctx.reply.Bulk(std::to_string(next));
  ctx.reply.ArrayHeader(page.items.size());
  for (const std::string& s : page.items) ctx.reply.Bulk(s);
}

// HSCAN/SSCAN key cursor [MATCH pattern] [COUNT count]. Redis 7.2 order: the cursor, then the
// key lookup (missing: cursor 0 and no elements; wrong type: WRONGTYPE), then the options.
void ElementScanCommand(ExecContext& ctx, const ArgList& argv, ValueType type) {
  uint64_t cursor = 0;
  if (!ParseScanCursor(argv[2], &cursor)) return ctx.reply.Error(kErrInvalidCursor);
  ScanOptions opts;
  Status opts_status = ParseScanOptions(argv, 3, /*allow_type=*/false, &opts);
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
  ReplyElementPage(ctx, page);
}

// A small zset (Redis listpack) comes back whole, in score order, with cursor 0; empty when it is large.
Status SmallZsetPage(ReadView& view, const Collection& c, const ScanOptions& opts, ElementPage* page,
                     bool* small) {
  *small = c.size() <= kZsetSmallMax;
  if (!*small) return Status::Ok();
  std::vector<std::string> items;
  Status st = ForEachByScore(view, c, false, [&](double s, std::string_view m) -> Result<bool> {
    if (m.size() > kZsetSmallValueMax) {
      *small = false;
      return false;
    }
    if (!opts.filter.pattern || GlobMatch(*opts.filter.pattern, m)) {
      items.emplace_back(m);
      items.push_back(FormatScore(s));
    }
    return true;
  });
  if (!st.ok() || !*small) return st;
  page->items = std::move(items);
  page->next_position.clear();
  return Status::Ok();
}

void ZScan(ExecContext& ctx, const ArgList& argv) {
  uint64_t cursor = 0;
  if (!ParseScanCursor(argv[2], &cursor)) return ctx.reply.Error(kErrInvalidCursor);
  ScanOptions opts;
  Status opts_status = ParseScanOptions(argv, 3, /*allow_type=*/false, &opts);
  const std::optional<ScanPosition> saved =
      cursor == 0 ? std::nullopt : ctx.server.cursors().Load(cursor, CursorKind::kElements);
  ElementPage page;
  const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) -> Status {
    page = ElementPage{};
    auto c = LoadCollection(view, ctx.session.db, argv[1], encoding::ValueType::kZSet, now_ms);
    if (!c.ok()) return c.status();
    if (!c->exists()) return Status::Ok();
    if (!opts_status.ok()) return opts_status;
    bool small = false;
    if (Status st = SmallZsetPage(view, *c, opts, &page, &small); !st.ok() || small) return st;
    const std::string prefix = c->Prefix();
    const ScanPosition position = saved && saved->key.starts_with(prefix) ? *saved : ScanPosition{};
    if (Status st = ScanElements(view, prefix, position, opts.count, opts.filter.pattern, true, &page);
        !st.ok())
      return st;
    for (size_t i = 1; i < page.items.size(); i += 2) {  // raw 8-byte scores -> %.17Lg text
      if (page.items[i].size() != 8) return Status::Corruption("bad zset score");
      page.items[i] = FormatScore17g(DecodeRawScore(page.items[i]));
    }
    return Status::Ok();
  });
  if (!ok) return;
  ReplyElementPage(ctx, page);
}

void HScan(ExecContext& ctx, const ArgList& argv) { ElementScanCommand(ctx, argv, ValueType::kHash); }
void SScan(ExecContext& ctx, const ArgList& argv) { ElementScanCommand(ctx, argv, ValueType::kSet); }

}  // namespace

void RegisterElementScanCommands(CommandRegistry& r) {
  r.Register({"hscan", -3, kCmdReadOnly, 1, 1, 1, &HScan});
  r.Register({"sscan", -3, kCmdReadOnly, 1, 1, 1, &SScan});
  r.Register({"zscan", -3, kCmdReadOnly, 1, 1, 1, &ZScan});
}

}  // namespace tideskv
