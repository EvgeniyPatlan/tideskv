#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "commands/all_commands.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "common/string_util.h"
#include "encoding/key_codec.h"
#include "exec/engine.h"
#include "types/cursor_store.h"
#include "types/scan.h"

namespace tideskv {
namespace {

struct ScanArgs {
  std::string position;  // where to resume; empty = start
  size_t count = 10;
  ScanFilter filter;
};

// Redis 7.2 scanGenericCommand option parsing (argv[2..]).
Status ParseScanOptions(const ArgList& argv, ScanArgs* out) {
  for (size_t i = 2; i < argv.size(); i += 2) {
    if (i + 1 >= argv.size()) return Status::UserError(std::string(kErrSyntax));
    const std::string& value = argv[i + 1];
    if (EqualsIgnoreCase(argv[i], "COUNT")) {
      int64_t n = 0;
      if (!ParseInt64(value, &n)) return Status::UserError(std::string(kErrNotInteger));
      if (n < 1) return Status::UserError(std::string(kErrSyntax));
      out->count = static_cast<size_t>(n);
    } else if (EqualsIgnoreCase(argv[i], "MATCH")) {
      out->filter.pattern = value == "*" ? std::string_view() : std::string_view(value);
    } else if (EqualsIgnoreCase(argv[i], "TYPE")) {
      out->filter.type = TypeFromName(value);
      out->filter.match_nothing = !out->filter.type.has_value();
    } else {
      return Status::UserError(std::string(kErrSyntax));
    }
  }
  return Status::Ok();
}

// Redis 7.2 parseScanCursorOrReply: strtoul semantics, so '-1' wraps, '+5', '007' and ''
// parse, while leading whitespace, trailing junk and out-of-range values are rejected.
bool ParseScanCursor(const std::string& arg, uint64_t* cursor) {
  const char* begin = arg.c_str();
  if (std::isspace(static_cast<unsigned char>(begin[0]))) return false;
  char* end = nullptr;
  errno = 0;
  const unsigned long long v = std::strtoull(begin, &end, 10);
  if (*end != '\0' || errno == ERANGE) return false;
  *cursor = v;
  return true;
}

// Resolves the cursor to a position inside the current db (D9: unknown or foreign = restart).
Status ResolveCursor(ExecContext& ctx, const std::string& arg, std::string* position) {
  uint64_t cursor = 0;
  if (!ParseScanCursor(arg, &cursor)) return Status::UserError(std::string(kErrInvalidCursor));
  position->clear();
  if (cursor == 0) return Status::Ok();
  std::optional<std::string> saved = ctx.server.cursors().Load(cursor);
  if (saved && saved->rfind(encoding::DbPrefix(ctx.session.db), 0) == 0) *position = std::move(*saved);
  return Status::Ok();
}

void Scan(ExecContext& ctx, const ArgList& argv) {
  ScanArgs args;
  if (Status st = ResolveCursor(ctx, argv[1], &args.position); !st.ok()) return ReplyError(ctx, st);
  if (Status st = ParseScanOptions(argv, &args); !st.ok()) return ReplyError(ctx, st);
  Engine& e = ctx.server.engine();
  const int64_t now = e.NowMs();
  ScanPage page;
  const Status st = e.Snapshot([&](ReadView& v) {
    return ScanKeys(v, ctx.session.db, args.position, args.count, args.filter, now, &page);
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
  const int64_t now = e.NowMs();
  std::vector<std::string> keys;
  const Status st =
      e.Snapshot([&](ReadView& v) { return MatchingKeys(v, ctx.session.db, argv[1], now, &keys); });
  if (!st.ok()) return ReplyError(ctx, st);
  ctx.reply.ArrayHeader(keys.size());
  for (const std::string& k : keys) ctx.reply.Bulk(k);
}

uint64_t NextRandom() {
  thread_local std::mt19937_64 rng{std::random_device{}()};
  return rng();
}

void RandomKeyCmd(ExecContext& ctx, const ArgList&) {
  Engine& e = ctx.server.engine();
  const int64_t now = e.NowMs();
  std::optional<std::string> key;
  const Status st = e.Snapshot(
      [&](ReadView& v) { return MoveInto(RandomKey(v, ctx.session.db, now, NextRandom()), &key); });
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
