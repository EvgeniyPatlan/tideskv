#include <cstdint>
#include <limits>
#include <optional>
#include <string>

#include "commands/all_commands.h"
#include "commands/cmd_util.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "common/number_format.h"
#include "common/string_util.h"
#include "exec/engine.h"
#include "types/errors.h"
#include "types/object_store.h"
#include "types/string_type.h"

namespace tideskv {
namespace {

void IncrGeneric(ExecContext& ctx, const std::string& key, int64_t delta) {
  int64_t value = 0;
  const bool ok = WriteOneKey(ctx, key, [&](WriteTxn& txn, int64_t now_ms) {
    value = 0;
    return MoveInto(StringIncrBy(txn, ctx.session.db, key, delta, now_ms), &value);
  });
  if (ok) ctx.reply.Integer(value);
}

void Incr(ExecContext& ctx, const ArgList& argv) { IncrGeneric(ctx, argv[1], 1); }
void Decr(ExecContext& ctx, const ArgList& argv) { IncrGeneric(ctx, argv[1], -1); }

void IncrBy(ExecContext& ctx, const ArgList& argv) {
  int64_t delta = 0;
  if (!ParseInt64(argv[2], &delta)) return ctx.reply.Error(kErrNotInteger);
  IncrGeneric(ctx, argv[1], delta);
}

void DecrBy(ExecContext& ctx, const ArgList& argv) {
  int64_t delta = 0;
  if (!ParseInt64(argv[2], &delta)) return ctx.reply.Error(kErrNotInteger);
  if (delta == std::numeric_limits<int64_t>::min()) return ctx.reply.Error(kErrDecrementOverflow);
  IncrGeneric(ctx, argv[1], -delta);
}

// Redis checks the key type before the increment argument, so a bad increment on a live
// non-string key still answers WRONGTYPE.
void ReplyBadFloatArgument(ExecContext& ctx, const std::string& key) {
  std::optional<std::string> ignored;
  if (Status st = ReadStringValue(ctx, key, &ignored); !st.ok()) return ReplyError(ctx, st);
  ctx.reply.Error(kErrNotFloat);
}

void IncrByFloat(ExecContext& ctx, const ArgList& argv) {
  long double delta = 0;
  if (!ParseLongDouble(argv[2], &delta)) return ReplyBadFloatArgument(ctx, argv[1]);
  std::string value;
  const bool ok = WriteOneKey(ctx, argv[1], [&](WriteTxn& txn, int64_t now_ms) {
    value.clear();
    return MoveInto(StringIncrByFloat(txn, ctx.session.db, argv[1], delta, now_ms), &value);
  });
  if (ok) ctx.reply.Bulk(value);
}

}  // namespace

void RegisterCounterCommands(CommandRegistry& r) {
  r.Register({"incr", 2, kCmdWrite | kCmdFast, 1, 1, 1, &Incr});
  r.Register({"decr", 2, kCmdWrite | kCmdFast, 1, 1, 1, &Decr});
  r.Register({"incrby", 3, kCmdWrite | kCmdFast, 1, 1, 1, &IncrBy});
  r.Register({"decrby", 3, kCmdWrite | kCmdFast, 1, 1, 1, &DecrBy});
  r.Register({"incrbyfloat", 3, kCmdWrite | kCmdFast, 1, 1, 1, &IncrByFloat});
}

}  // namespace tideskv
