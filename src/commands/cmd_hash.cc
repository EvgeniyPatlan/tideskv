#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "commands/all_commands.h"
#include "commands/cmd_util.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "common/number_format.h"
#include "common/string_util.h"
#include "exec/engine.h"
#include "types/hash_type.h"

namespace tideskv {
namespace {

std::vector<std::string_view> ArgsFrom(const ArgList& argv, size_t first) {
  return std::vector<std::string_view>(argv.begin() + static_cast<std::ptrdiff_t>(first), argv.end());
}

void HSetGeneric(ExecContext& ctx, const ArgList& argv, bool hmset) {
  if (argv.size() % 2 == 1) return ctx.reply.Error(ArityError(hmset ? "hmset" : "hset"));
  std::vector<FieldValue> pairs;
  for (size_t i = 2; i + 1 < argv.size(); i += 2) pairs.emplace_back(argv[i], argv[i + 1]);
  int64_t added = 0;
  const bool ok = WriteOneKey(ctx, argv[1], [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(HashSet(txn, ctx.server.oids(), ctx.session.db, argv[1], pairs, now_ms), &added);
  });
  if (!ok) return;
  hmset ? ctx.reply.SimpleString("OK") : ctx.reply.Integer(added);
}

void HSet(ExecContext& ctx, const ArgList& argv) { HSetGeneric(ctx, argv, false); }
void HMSet(ExecContext& ctx, const ArgList& argv) { HSetGeneric(ctx, argv, true); }

void HSetNx(ExecContext& ctx, const ArgList& argv) {
  bool created = false;
  const bool ok = WriteOneKey(ctx, argv[1], [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(HashSetNx(txn, ctx.server.oids(), ctx.session.db, argv[1], argv[2], argv[3], now_ms),
                    &created);
  });
  if (ok) ctx.reply.Integer(created ? 1 : 0);
}

void HGet(ExecContext& ctx, const ArgList& argv) {
  std::optional<std::string> v;
  const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) {
    return MoveInto(HashGet(view, ctx.session.db, argv[1], argv[2], now_ms), &v);
  });
  if (ok) ReplyOptional(ctx, v);
}

void HMGet(ExecContext& ctx, const ArgList& argv) {
  const auto fields = ArgsFrom(argv, 2);
  std::vector<std::optional<std::string>> values;
  const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) {
    return MoveInto(HashMultiGet(view, ctx.session.db, argv[1], fields, now_ms), &values);
  });
  if (!ok) return;
  ctx.reply.ArrayHeader(values.size());
  for (const auto& v : values) ReplyOptional(ctx, v);
}

void HDel(ExecContext& ctx, const ArgList& argv) {
  const auto fields = ArgsFrom(argv, 2);
  int64_t n = 0;
  const bool ok = WriteOneKey(ctx, argv[1], [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(HashDelete(txn, ctx.session.db, argv[1], fields, now_ms), &n);
  });
  if (ok) ctx.reply.Integer(n);
}

void HLen(ExecContext& ctx, const ArgList& argv) {
  int64_t n = 0;
  const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) {
    return MoveInto(HashLen(view, ctx.session.db, argv[1], now_ms), &n);
  });
  if (ok) ctx.reply.Integer(n);
}

void HStrlen(ExecContext& ctx, const ArgList& argv) {
  int64_t n = 0;
  const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) {
    return MoveInto(HashStrlen(view, ctx.session.db, argv[1], argv[2], now_ms), &n);
  });
  if (ok) ctx.reply.Integer(n);
}

void HExists(ExecContext& ctx, const ArgList& argv) {
  bool exists = false;
  const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) {
    return MoveInto(HashExists(view, ctx.session.db, argv[1], argv[2], now_ms), &exists);
  });
  if (ok) ctx.reply.Integer(exists ? 1 : 0);
}

void HGetAllGeneric(ExecContext& ctx, const ArgList& argv, HashPart part) {
  std::vector<std::string> items;
  const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) {
    return MoveInto(HashGetAll(view, ctx.session.db, argv[1], part, now_ms), &items);
  });
  if (!ok) return;
  // Redis genericHgetallCommand: a map for HGETALL (flat array in RESP2), an array otherwise.
  part == HashPart::kBoth ? ctx.reply.MapHeader(items.size() / 2) : ctx.reply.ArrayHeader(items.size());
  for (const std::string& s : items) ctx.reply.Bulk(s);
}

void HKeys(ExecContext& ctx, const ArgList& argv) { HGetAllGeneric(ctx, argv, HashPart::kFields); }
void HVals(ExecContext& ctx, const ArgList& argv) { HGetAllGeneric(ctx, argv, HashPart::kValues); }
void HGetAll(ExecContext& ctx, const ArgList& argv) { HGetAllGeneric(ctx, argv, HashPart::kBoth); }

// Redis 7.2 hincrbyCommand: the increment is parsed before the key is looked up.
void HIncrBy(ExecContext& ctx, const ArgList& argv) {
  int64_t delta = 0;
  if (!ParseInt64(argv[3], &delta)) return ctx.reply.Error(kErrNotInteger);
  int64_t value = 0;
  const bool ok = WriteOneKey(ctx, argv[1], [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(HashIncrBy(txn, ctx.server.oids(), ctx.session.db, argv[1], argv[2], delta, now_ms),
                    &value);
  });
  if (ok) ctx.reply.Integer(value);
}

// Redis 7.2 hincrbyfloatCommand: parse, then reject NaN/Infinity, then look the key up.
void HIncrByFloat(ExecContext& ctx, const ArgList& argv) {
  long double delta = 0;
  if (!ParseLongDouble(argv[3], &delta)) return ctx.reply.Error(kErrNotFloat);
  if (std::isnan(delta) || std::isinf(delta)) return ctx.reply.Error(kErrValueNanOrInfinity);
  std::string value;
  const bool ok = WriteOneKey(ctx, argv[1], [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(HashIncrByFloat(txn, ctx.server.oids(), ctx.session.db, argv[1], argv[2], delta, now_ms),
                    &value);
  });
  if (ok) ctx.reply.Bulk(value);
}

}  // namespace

void RegisterHashCommands(CommandRegistry& r) {
  r.Register({"hset", -4, kCmdWrite | kCmdFast, 1, 1, 1, &HSet});
  r.Register({"hmset", -4, kCmdWrite | kCmdFast, 1, 1, 1, &HMSet});
  r.Register({"hsetnx", 4, kCmdWrite | kCmdFast, 1, 1, 1, &HSetNx});
  r.Register({"hget", 3, kCmdReadOnly | kCmdFast, 1, 1, 1, &HGet});
  r.Register({"hmget", -3, kCmdReadOnly | kCmdFast, 1, 1, 1, &HMGet});
  r.Register({"hdel", -3, kCmdWrite | kCmdFast, 1, 1, 1, &HDel});
  r.Register({"hlen", 2, kCmdReadOnly | kCmdFast, 1, 1, 1, &HLen});
  r.Register({"hstrlen", 3, kCmdReadOnly | kCmdFast, 1, 1, 1, &HStrlen});
  r.Register({"hexists", 3, kCmdReadOnly | kCmdFast, 1, 1, 1, &HExists});
  r.Register({"hkeys", 2, kCmdReadOnly, 1, 1, 1, &HKeys});
  r.Register({"hvals", 2, kCmdReadOnly, 1, 1, 1, &HVals});
  r.Register({"hgetall", 2, kCmdReadOnly, 1, 1, 1, &HGetAll});
  r.Register({"hincrby", 4, kCmdWrite | kCmdFast, 1, 1, 1, &HIncrBy});
  r.Register({"hincrbyfloat", 4, kCmdWrite | kCmdFast, 1, 1, 1, &HIncrByFloat});
}

}  // namespace tideskv
