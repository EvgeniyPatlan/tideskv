#include <string>
#include <vector>

#include "commands/all_commands.h"
#include "commands/cmd_util.h"
#include "commands/count_args.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "common/string_util.h"
#include "types/set_algebra.h"

namespace tideskv {
namespace {

void Combine(ExecContext& ctx, const ArgList& argv, SetOp op) {
  const auto keys = ArgsFrom(argv, 1, argv.size());
  std::vector<std::string> members;
  const bool ok = RunSnapshot(ctx, [&](ReadView& view, int64_t now_ms) {
    return MoveInto(SetCombine(view, ctx.session.db, op, keys, now_ms), &members);
  });
  if (!ok) return;
  ctx.reply.SetHeader(members.size());
  for (const std::string& m : members) ctx.reply.Bulk(m);
}

void CombineStore(ExecContext& ctx, const ArgList& argv, SetOp op) {
  const auto locked = ArgsFrom(argv, 1, argv.size());  // dst and every source
  const auto keys = ArgsFrom(argv, 2, argv.size());
  int64_t n = 0;
  const Status st = ctx.server.engine().Write(ctx.session.db, locked, [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(SetCombineStore(txn, ctx.server.oids(), ctx.session.db, op, argv[1], keys, now_ms), &n);
  });
  if (!st.ok()) return ReplyError(ctx, st);
  ctx.reply.Integer(n);
}

void SInter(ExecContext& ctx, const ArgList& argv) { Combine(ctx, argv, SetOp::kInter); }
void SUnion(ExecContext& ctx, const ArgList& argv) { Combine(ctx, argv, SetOp::kUnion); }
void SDiff(ExecContext& ctx, const ArgList& argv) { Combine(ctx, argv, SetOp::kDiff); }
void SInterStore(ExecContext& ctx, const ArgList& argv) { CombineStore(ctx, argv, SetOp::kInter); }
void SUnionStore(ExecContext& ctx, const ArgList& argv) { CombineStore(ctx, argv, SetOp::kUnion); }
void SDiffStore(ExecContext& ctx, const ArgList& argv) { CombineStore(ctx, argv, SetOp::kDiff); }

// Redis 7.2 sinterCardCommand: SINTERCARD numkeys key [key ...] [LIMIT limit].
void SInterCard(ExecContext& ctx, const ArgList& argv) {
  int64_t numkeys = 0;
  if (!ParseInt64(argv[1], &numkeys) || numkeys < 1) return ctx.reply.Error(kErrNumkeysNotPositive);
  if (static_cast<uint64_t>(numkeys) > argv.size() - 2) return ctx.reply.Error(kErrNumkeysTooMany);
  const size_t keys_end = 2 + static_cast<size_t>(numkeys);
  int64_t limit = 0;
  for (size_t j = keys_end; j < argv.size(); ++j) {
    if (EqualsIgnoreCase(argv[j], "LIMIT") && j + 1 < argv.size()) {
      if (Status st = ParsePositiveCount(argv[++j], kErrLimitNegative, &limit); !st.ok())
        return ReplyError(ctx, st);
    } else {
      return ctx.reply.Error(kErrSyntax);
    }
  }
  const auto keys = ArgsFrom(argv, 2, keys_end);
  int64_t n = 0;
  const bool ok = RunSnapshot(ctx, [&](ReadView& view, int64_t now_ms) {
    return MoveInto(SetInterCard(view, ctx.session.db, keys, static_cast<uint64_t>(limit), now_ms), &n);
  });
  if (ok) ctx.reply.Integer(n);
}

}  // namespace

void RegisterSetAlgebraCommands(CommandRegistry& r) {
  r.Register({"sinter", -2, kCmdReadOnly, 1, -1, 1, &SInter});
  r.Register({"sunion", -2, kCmdReadOnly, 1, -1, 1, &SUnion});
  r.Register({"sdiff", -2, kCmdReadOnly, 1, -1, 1, &SDiff});
  r.Register({"sinterstore", -3, kCmdWrite, 1, -1, 1, &SInterStore});
  r.Register({"sunionstore", -3, kCmdWrite, 1, -1, 1, &SUnionStore});
  r.Register({"sdiffstore", -3, kCmdWrite, 1, -1, 1, &SDiffStore});
  // numkeys-based ("movable") keys; the triple cannot describe them. M7 routing extracts them.
  r.Register({"sintercard", -3, kCmdReadOnly, 0, 0, 0, &SInterCard});
}

}  // namespace tideskv
