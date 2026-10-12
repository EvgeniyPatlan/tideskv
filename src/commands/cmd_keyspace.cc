#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "commands/all_commands.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "common/string_util.h"
#include "encoding/key_codec.h"
#include "exec/engine.h"
#include "types/keyspace.h"

namespace tideskv {
namespace {

std::vector<std::string_view> KeysFrom(const ArgList& argv, size_t first) {
  return std::vector<std::string_view>(argv.begin() + static_cast<std::ptrdiff_t>(first), argv.end());
}

void Del(ExecContext& ctx, const ArgList& argv) {
  Engine& e = ctx.server.engine();
  const auto keys = KeysFrom(argv, 1);
  int64_t n = 0;
  const Status st = e.Write(ctx.session.db, keys, [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(DeleteKeys(txn, ctx.session.db, keys, now_ms), &n);
  });
  if (!st.ok()) return ReplyError(ctx, st);
  ctx.reply.Integer(n);
}

void Exists(ExecContext& ctx, const ArgList& argv) {
  Engine& e = ctx.server.engine();
  const auto keys = KeysFrom(argv, 1);
  int64_t n = 0;
  const Status st = e.Snapshot([&](ReadView& v, int64_t now_ms) {
    return MoveInto(CountExisting(v, ctx.session.db, keys, now_ms), &n);
  });
  if (!st.ok()) return ReplyError(ctx, st);
  ctx.reply.Integer(n);
}

void Type(ExecContext& ctx, const ArgList& argv) {
  Engine& e = ctx.server.engine();
  std::string type;
  const Status st = e.Read([&](ReadView& v, int64_t now_ms) {
    return MoveInto(TypeOf(v, ctx.session.db, argv[1], now_ms), &type);
  });
  if (!st.ok()) return ReplyError(ctx, st);
  ctx.reply.SimpleString(type);
}

void Select(ExecContext& ctx, const ArgList& argv) {
  int64_t db = 0;
  if (!ParseInt64(argv[1], &db) || db < std::numeric_limits<int32_t>::min() ||
      db > std::numeric_limits<int32_t>::max()) {
    return ctx.reply.Error(kErrNotInteger);  // Redis getIntFromObjectOrReply: must fit int32
  }
  if (db < 0 || db >= encoding::kNumDatabases) return ctx.reply.Error(kErrDbIndexOutOfRange);
  ctx.session.db = static_cast<uint8_t>(db);
  ctx.reply.SimpleString("OK");
}

void DbSize(ExecContext& ctx, const ArgList&) {
  Engine& e = ctx.server.engine();
  uint64_t n = 0;
  const Status st = e.Snapshot(
      [&](ReadView& v, int64_t now_ms) { return MoveInto(CountKeys(v, ctx.session.db, now_ms), &n); });
  if (!st.ok()) return ReplyError(ctx, st);
  ctx.reply.Integer(static_cast<int64_t>(n));
}

// FLUSHDB/FLUSHALL [ASYNC|SYNC]: both modes are O(1) prefix deletes here.
bool FlushArgsOk(const ArgList& argv) {
  return argv.size() == 1 ||
         (argv.size() == 2 && (EqualsIgnoreCase(argv[1], "ASYNC") || EqualsIgnoreCase(argv[1], "SYNC")));
}

void FlushDbCmd(ExecContext& ctx, const ArgList& argv) {
  if (!FlushArgsOk(argv)) return ctx.reply.Error(kErrSyntax);
  const uint8_t db = ctx.session.db;
  const Status st =
      ctx.server.engine().WriteExclusive([db](WriteTxn& txn, int64_t) { return FlushDb(txn, db); });
  if (!st.ok()) return ReplyError(ctx, st);
  ctx.reply.SimpleString("OK");
}

void FlushAllCmd(ExecContext& ctx, const ArgList& argv) {
  if (!FlushArgsOk(argv)) return ctx.reply.Error(kErrSyntax);
  const Status st = ctx.server.engine().WriteExclusive([](WriteTxn& txn, int64_t) { return FlushAll(txn); });
  if (!st.ok()) return ReplyError(ctx, st);
  ctx.reply.SimpleString("OK");
}

void RenameGeneric(ExecContext& ctx, const ArgList& argv, bool nx) {
  Engine& e = ctx.server.engine();
  int64_t result = 0;
  const Status st = e.Write(ctx.session.db, {argv[1], argv[2]}, [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(RenameKey(txn, ctx.session.db, argv[1], argv[2], nx, now_ms), &result);
  });
  if (!st.ok()) return ReplyError(ctx, st);
  nx ? ctx.reply.Integer(result) : ctx.reply.SimpleString("OK");
}

void Rename(ExecContext& ctx, const ArgList& argv) { RenameGeneric(ctx, argv, false); }
void RenameNx(ExecContext& ctx, const ArgList& argv) { RenameGeneric(ctx, argv, true); }

}  // namespace

void RegisterKeyspaceCommands(CommandRegistry& r) {
  r.Register({"del", -2, kCmdWrite, 1, -1, 1, &Del});
  r.Register({"unlink", -2, kCmdWrite | kCmdFast, 1, -1, 1, &Del});
  r.Register({"exists", -2, kCmdReadOnly | kCmdFast, 1, -1, 1, &Exists});
  r.Register({"type", 2, kCmdReadOnly | kCmdFast, 1, 1, 1, &Type});
  r.Register({"select", 2, kCmdFast, 0, 0, 0, &Select});
  r.Register({"dbsize", 1, kCmdReadOnly | kCmdFast, 0, 0, 0, &DbSize});
  r.Register({"flushdb", -1, kCmdWrite, 0, 0, 0, &FlushDbCmd});
  r.Register({"flushall", -1, kCmdWrite, 0, 0, 0, &FlushAllCmd});
  r.Register({"rename", 3, kCmdWrite, 1, 2, 1, &Rename});
  r.Register({"renamenx", 3, kCmdWrite | kCmdFast, 1, 2, 1, &RenameNx});
}

}  // namespace tideskv
