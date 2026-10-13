#include <optional>
#include <string>
#include <vector>

#include "commands/all_commands.h"
#include "commands/cmd_util.h"
#include "commands/count_args.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "common/string_util.h"
#include "types/list_type.h"

namespace tideskv {
namespace {

void ReplyStrings(ExecContext& ctx, const std::vector<std::string>& items) {
  ctx.reply.ArrayHeader(items.size());
  for (const std::string& s : items) ctx.reply.Bulk(s);
}

void PushCommand(ExecContext& ctx, const ArgList& argv, ListEnd end, bool only_if_exists) {
  const auto values = ArgsFrom(argv, 2);
  int64_t len = 0;
  const bool ok = WriteOneKey(ctx, argv[1], [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(
        ListPush(txn, ctx.server.oids(), ctx.session.db, argv[1], end, values, only_if_exists, now_ms), &len);
  });
  if (ok) ctx.reply.Integer(len);
}

void LPush(ExecContext& ctx, const ArgList& argv) { PushCommand(ctx, argv, ListEnd::kLeft, false); }
void RPush(ExecContext& ctx, const ArgList& argv) { PushCommand(ctx, argv, ListEnd::kRight, false); }
void LPushX(ExecContext& ctx, const ArgList& argv) { PushCommand(ctx, argv, ListEnd::kLeft, true); }
void RPushX(ExecContext& ctx, const ArgList& argv) { PushCommand(ctx, argv, ListEnd::kRight, true); }

// Redis 7.2 popGenericCommand: the count is validated before the key is looked up. A missing key
// is a nil bulk without a count and a null array with one; the count form is always an array.
void PopCommand(ExecContext& ctx, const ArgList& argv, ListEnd end, std::string_view name) {
  if (argv.size() > 3) return ctx.reply.Error(ArityError(name));
  const bool has_count = argv.size() == 3;
  int64_t count = 1;
  if (has_count) {
    if (Status st = ParseNonNegativeCount(argv[2], &count); !st.ok()) return ReplyError(ctx, st);
  }
  std::optional<std::vector<std::string>> popped;
  const bool ok = WriteOneKey(ctx, argv[1], [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(ListPop(txn, ctx.session.db, argv[1], end, static_cast<uint64_t>(count), now_ms),
                    &popped);
  });
  if (!ok) return;
  if (!popped) return has_count ? ctx.reply.NullArray() : ctx.reply.Null();
  if (!has_count) return ctx.reply.Bulk(popped->front());
  ReplyStrings(ctx, *popped);
}

void LPop(ExecContext& ctx, const ArgList& argv) { PopCommand(ctx, argv, ListEnd::kLeft, "lpop"); }
void RPop(ExecContext& ctx, const ArgList& argv) { PopCommand(ctx, argv, ListEnd::kRight, "rpop"); }

void LLen(ExecContext& ctx, const ArgList& argv) {
  int64_t n = 0;
  const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) {
    return MoveInto(ListLen(view, ctx.session.db, argv[1], now_ms), &n);
  });
  if (ok) ctx.reply.Integer(n);
}

void LIndex(ExecContext& ctx, const ArgList& argv) {
  std::optional<std::string> v;
  const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) {
    return MoveInto(ListIndex(view, ctx.session.db, argv[1], argv[2], now_ms), &v);
  });
  if (ok) ReplyOptional(ctx, v);
}

void LSet(ExecContext& ctx, const ArgList& argv) {
  const bool ok = WriteOneKey(ctx, argv[1], [&](WriteTxn& txn, int64_t now_ms) {
    return ListSet(txn, ctx.session.db, argv[1], argv[2], argv[3], now_ms);
  });
  if (ok) ctx.reply.SimpleString("OK");
}

// start and stop are parsed before the key is looked up (bad integers beat WRONGTYPE).
void LRange(ExecContext& ctx, const ArgList& argv) {
  int64_t start = 0, stop = 0;
  if (!ParseInt64(argv[2], &start) || !ParseInt64(argv[3], &stop)) return ctx.reply.Error(kErrNotInteger);
  std::vector<std::string> items;
  const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) {
    return MoveInto(ListRange(view, ctx.session.db, argv[1], start, stop, now_ms), &items);
  });
  if (ok) ReplyStrings(ctx, items);
}

}  // namespace

void RegisterListCommands(CommandRegistry& r) {
  r.Register({"lpush", -3, kCmdWrite | kCmdFast, 1, 1, 1, &LPush});
  r.Register({"rpush", -3, kCmdWrite | kCmdFast, 1, 1, 1, &RPush});
  r.Register({"lpushx", -3, kCmdWrite | kCmdFast, 1, 1, 1, &LPushX});
  r.Register({"rpushx", -3, kCmdWrite | kCmdFast, 1, 1, 1, &RPushX});
  r.Register({"lpop", -2, kCmdWrite | kCmdFast, 1, 1, 1, &LPop});
  r.Register({"rpop", -2, kCmdWrite | kCmdFast, 1, 1, 1, &RPop});
  r.Register({"llen", 2, kCmdReadOnly | kCmdFast, 1, 1, 1, &LLen});
  r.Register({"lindex", 3, kCmdReadOnly, 1, 1, 1, &LIndex});
  r.Register({"lset", 4, kCmdWrite, 1, 1, 1, &LSet});
  r.Register({"lrange", 4, kCmdReadOnly, 1, 1, 1, &LRange});
}

}  // namespace tideskv
