#include <string>

#include "commands/all_commands.h"
#include "commands/cmd_util.h"
#include "commands/expire_args.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "common/string_util.h"
#include "exec/engine.h"
#include "types/errors.h"
#include "types/keyspace.h"

namespace tideskv {
namespace {

// Redis 7.2 parseExtendedExpireArgumentsOrReply.
Result<ExpireCondition> ParseCondition(const ArgList& argv) {
  ExpireCondition c;
  for (size_t j = 3; j < argv.size(); ++j) {
    if (EqualsIgnoreCase(argv[j], "NX")) {
      c.nx = true;
    } else if (EqualsIgnoreCase(argv[j], "XX")) {
      c.xx = true;
    } else if (EqualsIgnoreCase(argv[j], "GT")) {
      c.gt = true;
    } else if (EqualsIgnoreCase(argv[j], "LT")) {
      c.lt = true;
    } else {
      return Status::UserError(UnsupportedOptionError(argv[j]));
    }
  }
  if (c.nx && (c.xx || c.gt || c.lt)) return Status::UserError(std::string(kErrExpireNxCombo));
  if (c.gt && c.lt) return Status::UserError(std::string(kErrExpireGtLt));
  return c;
}

void ExpireGeneric(ExecContext& ctx, const ArgList& argv, ExpireUnit unit, std::string_view name) {
  auto cond = ParseCondition(argv);
  if (!cond.ok()) return ReplyError(ctx, cond.status());
  Engine& e = ctx.server.engine();
  // Parse-time base for a relative deadline (as Redis does at command start); whether the
  // deadline has already passed is judged with the Engine's under-lock now_ms.
  auto when = ParseExpireCommandTime(argv[2], unit, e.NowMs(), name);
  if (!when.ok()) return ReplyError(ctx, when.status());
  int64_t result = 0;
  const Status st = e.Write(ctx.session.db, {argv[1]}, [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(SetKeyExpire(txn, ctx.session.db, argv[1], *when, *cond, now_ms), &result);
  });
  if (!st.ok()) return ReplyError(ctx, st);
  ctx.reply.Integer(result);
}

void Expire(ExecContext& ctx, const ArgList& a) { ExpireGeneric(ctx, a, ExpireUnit::kSeconds, "expire"); }
void PExpire(ExecContext& ctx, const ArgList& a) { ExpireGeneric(ctx, a, ExpireUnit::kMillis, "pexpire"); }
void ExpireAt(ExecContext& ctx, const ArgList& a) {
  ExpireGeneric(ctx, a, ExpireUnit::kUnixSeconds, "expireat");
}
void PExpireAt(ExecContext& ctx, const ArgList& a) {
  ExpireGeneric(ctx, a, ExpireUnit::kUnixMillis, "pexpireat");
}

void TtlGeneric(ExecContext& ctx, const ArgList& argv, bool millis, bool absolute) {
  Engine& e = ctx.server.engine();
  int64_t ms = 0;
  const Status st = e.Read([&](ReadView& v, int64_t now_ms) {
    return MoveInto(KeyTtlMs(v, ctx.session.db, argv[1], absolute, now_ms), &ms);
  });
  if (!st.ok()) return ReplyError(ctx, st);
  ctx.reply.Integer(ms < 0 || millis ? ms : (ms + 500) / 1000);
}

void Ttl(ExecContext& ctx, const ArgList& a) { TtlGeneric(ctx, a, false, false); }
void PTtl(ExecContext& ctx, const ArgList& a) { TtlGeneric(ctx, a, true, false); }
void ExpireTime(ExecContext& ctx, const ArgList& a) { TtlGeneric(ctx, a, false, true); }
void PExpireTime(ExecContext& ctx, const ArgList& a) { TtlGeneric(ctx, a, true, true); }

void Persist(ExecContext& ctx, const ArgList& argv) {
  int64_t result = 0;
  const bool ok = WriteOneKey(ctx, argv[1], [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(PersistKey(txn, ctx.session.db, argv[1], now_ms), &result);
  });
  if (ok) ctx.reply.Integer(result);
}

}  // namespace

void RegisterExpireCommands(CommandRegistry& r) {
  r.Register({"expire", -3, kCmdWrite | kCmdFast, 1, 1, 1, &Expire});
  r.Register({"pexpire", -3, kCmdWrite | kCmdFast, 1, 1, 1, &PExpire});
  r.Register({"expireat", -3, kCmdWrite | kCmdFast, 1, 1, 1, &ExpireAt});
  r.Register({"pexpireat", -3, kCmdWrite | kCmdFast, 1, 1, 1, &PExpireAt});
  r.Register({"ttl", 2, kCmdReadOnly | kCmdFast, 1, 1, 1, &Ttl});
  r.Register({"pttl", 2, kCmdReadOnly | kCmdFast, 1, 1, 1, &PTtl});
  r.Register({"expiretime", 2, kCmdReadOnly | kCmdFast, 1, 1, 1, &ExpireTime});
  r.Register({"pexpiretime", 2, kCmdReadOnly | kCmdFast, 1, 1, 1, &PExpireTime});
  r.Register({"persist", 2, kCmdWrite | kCmdFast, 1, 1, 1, &Persist});
}

}  // namespace tideskv
