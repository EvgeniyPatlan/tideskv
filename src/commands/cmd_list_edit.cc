#include <optional>
#include <string>
#include <vector>

#include "commands/all_commands.h"
#include "commands/cmd_util.h"
#include "commands/count_args.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "common/string_util.h"
#include "types/list_edit.h"

namespace tideskv {
namespace {

// Redis 7.2 linsertCommand: the BEFORE/AFTER token is checked before the key.
void LInsert(ExecContext& ctx, const ArgList& argv) {
  InsertWhere where;
  if (EqualsIgnoreCase(argv[2], "before")) {
    where = InsertWhere::kBefore;
  } else if (EqualsIgnoreCase(argv[2], "after")) {
    where = InsertWhere::kAfter;
  } else {
    return ctx.reply.Error(kErrSyntax);
  }
  int64_t n = 0;
  const bool ok = WriteOneKey(ctx, argv[1], [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(ListInsert(txn, ctx.session.db, argv[1], where, argv[3], argv[4], now_ms), &n);
  });
  if (ok) ctx.reply.Integer(n);
}

void LRem(ExecContext& ctx, const ArgList& argv) {
  int64_t count = 0;
  if (!ParseInt64(argv[2], &count)) return ctx.reply.Error(kErrNotInteger);
  int64_t n = 0;
  const bool ok = WriteOneKey(ctx, argv[1], [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(ListRemove(txn, ctx.session.db, argv[1], count, argv[3], now_ms), &n);
  });
  if (ok) ctx.reply.Integer(n);
}

void LTrim(ExecContext& ctx, const ArgList& argv) {
  int64_t start = 0, stop = 0;
  if (!ParseInt64(argv[2], &start) || !ParseInt64(argv[3], &stop)) return ctx.reply.Error(kErrNotInteger);
  const bool ok = WriteOneKey(ctx, argv[1], [&](WriteTxn& txn, int64_t now_ms) {
    return ListTrim(txn, ctx.session.db, argv[1], start, stop, now_ms);
  });
  if (ok) ctx.reply.SimpleString("OK");
}

// Redis 7.2 lposCommand option loop (redis72-list.md §2.10): every option needs a value.
Status ParseLposOptions(const ArgList& argv, ListPosOptions* o) {
  for (size_t i = 3; i < argv.size(); i += 2) {
    if (i + 1 >= argv.size()) return Status::UserError(std::string(kErrSyntax));
    const std::string& v = argv[i + 1];
    if (EqualsIgnoreCase(argv[i], "RANK")) {
      if (!ParseInt64(v, &o->rank)) return Status::UserError(std::string(kErrNotInteger));
      if (o->rank == INT64_MIN) return Status::UserError(std::string(kErrCountOutOfRange));
      if (o->rank == 0) return Status::UserError(std::string(kErrRankZero));
    } else if (EqualsIgnoreCase(argv[i], "COUNT")) {
      int64_t n = 0;
      if (Status st = ParsePositiveCount(v, kErrCountNegative, &n); !st.ok()) return st;
      o->count = n;
    } else if (EqualsIgnoreCase(argv[i], "MAXLEN")) {
      if (Status st = ParsePositiveCount(v, kErrMaxlenNegative, &o->maxlen); !st.ok()) return st;
    } else {
      return Status::UserError(std::string(kErrSyntax));
    }
  }
  return Status::Ok();
}

void LPos(ExecContext& ctx, const ArgList& argv) {
  ListPosOptions opts;
  if (Status st = ParseLposOptions(argv, &opts); !st.ok()) return ReplyError(ctx, st);
  std::optional<std::vector<int64_t>> found;
  const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) {
    return MoveInto(ListPos(view, ctx.session.db, argv[1], argv[2], opts, now_ms), &found);
  });
  if (!ok) return;
  if (!opts.count) {
    if (!found || found->empty()) return ctx.reply.Null();
    return ctx.reply.Integer(found->front());
  }
  const size_t n = found ? found->size() : 0;
  ctx.reply.ArrayHeader(n);
  for (size_t i = 0; i < n; ++i) ctx.reply.Integer((*found)[i]);
}

}  // namespace

void RegisterListEditCommands(CommandRegistry& r) {
  r.Register({"linsert", 5, kCmdWrite, 1, 1, 1, &LInsert});
  r.Register({"lrem", 4, kCmdWrite, 1, 1, 1, &LRem});
  r.Register({"ltrim", 4, kCmdWrite, 1, 1, 1, &LTrim});
  r.Register({"lpos", -3, kCmdReadOnly, 1, 1, 1, &LPos});
}

}  // namespace tideskv
