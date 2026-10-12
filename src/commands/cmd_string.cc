#include <optional>
#include <string>
#include <vector>

#include "commands/all_commands.h"
#include "commands/cmd_util.h"
#include "commands/expire_args.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "exec/engine.h"
#include "types/string_type.h"

namespace tideskv {
namespace {

void ReplyOptional(ExecContext& ctx, const std::optional<std::string>& v) {
  v ? ctx.reply.Bulk(*v) : ctx.reply.Null();
}

// Runs StringSet for one key and replies like SET (OK / nil / old value with GET).
void DoSet(ExecContext& ctx, std::string_view key, std::string_view value, const SetOptions& opts) {
  SetResult result;
  const bool ok = WriteOneKey(ctx, key, [&](WriteTxn& txn, int64_t now_ms) {
    return StringSet(txn, ctx.session.db, key, value, opts, now_ms, &result);
  });
  if (!ok) return;
  if (opts.get) return ReplyOptional(ctx, result.old_value);
  result.written ? ctx.reply.SimpleString("OK") : ctx.reply.Null();
}

void Set(ExecContext& ctx, const ArgList& argv) {
  auto args = ParseExtendedArgs(argv, 3, ExtendedArgsKind::kSet);
  if (!args.ok()) return ReplyError(ctx, args.status());
  const int64_t now = ctx.server.engine().NowMs();
  SetOptions opts;
  opts.condition = args->condition;
  opts.get = args->get;
  opts.keep_ttl = args->keep_ttl;
  if (const std::optional<ExpireUnit> unit = args->unit) {
    auto when = ParsePositiveExpire(args->expire_arg, *unit, now, "set");
    if (!when.ok()) return ReplyError(ctx, when.status());
    opts.expire_ms = *when;
  }
  DoSet(ctx, argv[1], argv[2], opts);
}

void SetNx(ExecContext& ctx, const ArgList& argv) {
  SetOptions opts;
  opts.condition = SetOptions::Condition::kIfMissing;
  SetResult result;
  const bool ok = WriteOneKey(ctx, argv[1], [&](WriteTxn& txn, int64_t now_ms) {
    return StringSet(txn, ctx.session.db, argv[1], argv[2], opts, now_ms, &result);
  });
  if (ok) ctx.reply.Integer(result.written ? 1 : 0);
}

void SetExGeneric(ExecContext& ctx, const ArgList& argv, ExpireUnit unit, std::string_view name) {
  const int64_t now = ctx.server.engine().NowMs();
  auto when = ParsePositiveExpire(argv[2], unit, now, name);
  if (!when.ok()) return ReplyError(ctx, when.status());
  SetOptions opts;
  opts.expire_ms = *when;
  DoSet(ctx, argv[1], argv[3], opts);
}

void SetEx(ExecContext& ctx, const ArgList& argv) { SetExGeneric(ctx, argv, ExpireUnit::kSeconds, "setex"); }
void PSetEx(ExecContext& ctx, const ArgList& argv) { SetExGeneric(ctx, argv, ExpireUnit::kMillis, "psetex"); }

void GetSet(ExecContext& ctx, const ArgList& argv) {
  SetOptions opts;
  opts.get = true;
  DoSet(ctx, argv[1], argv[2], opts);
}

void Get(ExecContext& ctx, const ArgList& argv) {
  std::optional<std::string> value;
  if (Status st = ReadStringValue(ctx, argv[1], &value); !st.ok()) return ReplyError(ctx, st);
  ReplyOptional(ctx, value);
}

void GetDel(ExecContext& ctx, const ArgList& argv) {
  std::optional<std::string> value;
  const bool ok = WriteOneKey(ctx, argv[1], [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(StringGetDel(txn, ctx.session.db, argv[1], now_ms), &value);
  });
  if (ok) ReplyOptional(ctx, value);
}

void Strlen(ExecContext& ctx, const ArgList& argv) {
  std::optional<std::string> value;
  if (Status st = ReadStringValue(ctx, argv[1], &value); !st.ok()) return ReplyError(ctx, st);
  ctx.reply.Integer(value ? static_cast<int64_t>(value->size()) : 0);
}

void MGet(ExecContext& ctx, const ArgList& argv) {
  const std::vector<std::string_view> keys(argv.begin() + 1, argv.end());
  std::vector<std::optional<std::string>> values;
  const Status st = ctx.server.engine().Snapshot([&](ReadView& v, int64_t now_ms) {
    return MoveInto(StringMultiGet(v, ctx.session.db, keys, now_ms), &values);
  });
  if (!st.ok()) return ReplyError(ctx, st);
  ctx.reply.ArrayHeader(values.size());
  for (const auto& v : values) ReplyOptional(ctx, v);
}

void MSetGeneric(ExecContext& ctx, const ArgList& argv, bool nx) {
  if (argv.size() % 2 == 0) return ctx.reply.Error(ArityError(nx ? "msetnx" : "mset"));
  std::vector<KeyValue> pairs;
  std::vector<std::string_view> keys;
  for (size_t i = 1; i + 1 < argv.size(); i += 2) {
    pairs.emplace_back(argv[i], argv[i + 1]);
    keys.push_back(argv[i]);
  }
  bool written = false;
  const Status st = ctx.server.engine().Write(ctx.session.db, keys, [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(StringMultiSet(txn, ctx.session.db, pairs, nx, now_ms), &written);
  });
  if (!st.ok()) return ReplyError(ctx, st);
  nx ? ctx.reply.Integer(written ? 1 : 0) : ctx.reply.SimpleString("OK");
}

void MSet(ExecContext& ctx, const ArgList& argv) { MSetGeneric(ctx, argv, false); }
void MSetNx(ExecContext& ctx, const ArgList& argv) { MSetGeneric(ctx, argv, true); }

}  // namespace

void RegisterStringCommands(CommandRegistry& r) {
  r.Register({"get", 2, kCmdReadOnly | kCmdFast, 1, 1, 1, &Get});
  r.Register({"set", -3, kCmdWrite, 1, 1, 1, &Set});
  r.Register({"setnx", 3, kCmdWrite | kCmdFast, 1, 1, 1, &SetNx});
  r.Register({"setex", 4, kCmdWrite, 1, 1, 1, &SetEx});
  r.Register({"psetex", 4, kCmdWrite, 1, 1, 1, &PSetEx});
  r.Register({"getset", 3, kCmdWrite | kCmdFast, 1, 1, 1, &GetSet});
  r.Register({"getdel", 2, kCmdWrite | kCmdFast, 1, 1, 1, &GetDel});
  r.Register({"strlen", 2, kCmdReadOnly | kCmdFast, 1, 1, 1, &Strlen});
  r.Register({"mget", -2, kCmdReadOnly | kCmdFast, 1, -1, 1, &MGet});
  r.Register({"mset", -3, kCmdWrite, 1, -1, 2, &MSet});
  r.Register({"msetnx", -3, kCmdWrite, 1, -1, 2, &MSetNx});
}

}  // namespace tideskv
