#include <optional>
#include <string>

#include "commands/all_commands.h"
#include "commands/cmd_util.h"
#include "commands/expire_args.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "common/string_util.h"
#include "exec/engine.h"
#include "types/errors.h"
#include "types/string_type.h"

namespace tideskv {
namespace {

void GetEx(ExecContext& ctx, const ArgList& argv) {
  // Syntax only: Redis validates the expire value after the key lookup and type check.
  auto args = ParseExtendedArgs(argv, 2, ExtendedArgsKind::kGetEx);
  if (!args.ok()) return ReplyError(ctx, args.status());
  const int64_t now = ctx.server.engine().NowMs();  // parse-time base for a relative EX/PX
  const GetExResolver resolve = [&]() -> Result<GetExChange> {
    GetExChange change;
    if (args->persist) change.kind = GetExChange::Kind::kPersist;
    if (args->unit) {
      auto when = ParsePositiveExpire(args->expire_arg, *args->unit, now, "getex");
      if (!when.ok()) return when.status();
      change.kind = GetExChange::Kind::kSetExpire;
      change.expire_ms = *when;
    }
    return change;
  };
  std::optional<std::string> value;
  const bool ok = WriteOneKey(ctx, argv[1], [&](WriteTxn& txn, int64_t now_ms) {
    value.reset();
    return MoveInto(StringGetEx(txn, ctx.session.db, argv[1], resolve, now_ms), &value);
  });
  if (!ok) return;
  value ? ctx.reply.Bulk(*value) : ctx.reply.Null();
}

void Append(ExecContext& ctx, const ArgList& argv) {
  int64_t len = 0;
  // Redis signals on every non-error APPEND (t_string.c:723).
  std::vector<std::string> also;
  const bool ok = WriteOneKey(
      ctx, argv[1],
      [&](WriteTxn& txn, int64_t now_ms) {
        len = 0;
        Status s = MoveInto(StringAppend(txn, ctx.session.db, argv[1], argv[2], now_ms), &len);
        if (s.ok()) also.push_back(argv[1]);
        return s;
      },
      &also);
  if (ok) ctx.reply.Integer(len);
}

void SetRange(ExecContext& ctx, const ArgList& argv) {
  int64_t offset = 0;
  if (!ParseInt64(argv[2], &offset)) return ctx.reply.Error(kErrNotInteger);
  if (offset < 0) return ctx.reply.Error(kErrOffsetOutOfRange);
  int64_t len = 0;
  const bool ok = WriteOneKey(ctx, argv[1], [&](WriteTxn& txn, int64_t now_ms) {
    len = 0;
    return MoveInto(StringSetRange(txn, ctx.session.db, argv[1], offset, argv[3], now_ms), &len);
  });
  if (ok) ctx.reply.Integer(len);
}

void GetRange(ExecContext& ctx, const ArgList& argv) {
  int64_t start = 0, end = 0;
  if (!ParseInt64(argv[2], &start) || !ParseInt64(argv[3], &end)) return ctx.reply.Error(kErrNotInteger);
  std::optional<std::string> value;
  if (Status st = ReadStringValue(ctx, argv[1], &value); !st.ok()) return ReplyError(ctx, st);
  ctx.reply.Bulk(value ? RedisRange(*value, start, end) : std::string_view());
}

}  // namespace

void RegisterStringEditCommands(CommandRegistry& r) {
  r.Register({"getex", -2, kCmdWrite | kCmdFast, 1, 1, 1, &GetEx});
  r.Register({"append", 3, kCmdWrite | kCmdFast, 1, 1, 1, &Append});
  r.Register({"setrange", 4, kCmdWrite, 1, 1, 1, &SetRange});
  r.Register({"getrange", 4, kCmdReadOnly, 1, 1, 1, &GetRange});
  r.Register({"substr", 4, kCmdReadOnly, 1, 1, 1, &GetRange});
}

}  // namespace tideskv
