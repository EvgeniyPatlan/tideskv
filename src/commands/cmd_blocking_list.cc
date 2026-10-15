#include <optional>
#include <string>
#include <vector>

#include "commands/all_commands.h"
#include "commands/block_args.h"
#include "commands/cmd_util.h"
#include "commands/list_reply.h"
#include "commands/numkeys_args.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "types/list_move.h"

namespace tideskv {
namespace {

std::vector<std::string> Owned(const std::vector<std::string_view>& keys) {
  return std::vector<std::string>(keys.begin(), keys.end());
}

// Pops from the first list among `keys`. Answers on data or an error; otherwise parks (or, without a
// blocking context, answers a null array as Redis does under CLIENT_DENY_BLOCKING).
void BlockingPop(ExecContext& ctx, const std::vector<std::string_view>& keys, ListEnd end, uint64_t count,
                 int64_t timeout_ms, bool nested_reply) {
  std::optional<PoppedFrom> popped;
  const Status st = ctx.server.engine().Write(ctx.session.db, keys, [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(ListPopFirst(txn, ctx.session.db, keys, end, count, now_ms), &popped);
  });
  if (!st.ok()) return ReplyError(ctx, st);
  if (popped) {
    if (nested_reply) return ReplyPopped(ctx, popped);  // BLMPOP: [key, [elements]]
    ctx.reply.ArrayHeader(2);                           // BLPOP/BRPOP: [key, element]
    ctx.reply.Bulk(popped->key);
    return ctx.reply.Bulk(popped->values.front());
  }
  if (ctx.block == nullptr) return ctx.reply.NullArray();
  ctx.block->Block({encoding::ValueType::kList, Owned(keys), timeout_ms, {}});
}

// BLPOP/BRPOP key [key ...] timeout: the timeout is parsed first.
void BPopCommand(ExecContext& ctx, const ArgList& argv, ListEnd end) {
  int64_t timeout_ms = 0;
  if (Status st = ParseBlockTimeout(argv.back(), ctx.server.engine().NowMs(), &timeout_ms); !st.ok())
    return ReplyError(ctx, st);
  BlockingPop(ctx, ArgsFrom(argv, 1, argv.size() - 1), end, 1, timeout_ms, false);
}

void BLPop(ExecContext& ctx, const ArgList& argv) { BPopCommand(ctx, argv, ListEnd::kLeft); }
void BRPop(ExecContext& ctx, const ArgList& argv) { BPopCommand(ctx, argv, ListEnd::kRight); }

// BLMPOP timeout numkeys key [key ...] LEFT|RIGHT [COUNT count]: the mpop arguments, then the timeout.
void BLMPop(ExecContext& ctx, const ArgList& argv) {
  MultiPopArgs a;
  if (Status st = ParseMultiPop(argv, 2, "LEFT", "RIGHT", &a); !st.ok()) return ReplyError(ctx, st);
  int64_t timeout_ms = 0;
  if (Status st = ParseBlockTimeout(argv[1], ctx.server.engine().NowMs(), &timeout_ms); !st.ok())
    return ReplyError(ctx, st);
  BlockingPop(ctx, ArgsFrom(argv, a.first_key, a.first_key + a.num_keys),
              a.second ? ListEnd::kRight : ListEnd::kLeft, static_cast<uint64_t>(a.count), timeout_ms, true);
}

// BLMOVE/BRPOPLPUSH: src exists -> exactly LMOVE (dst type checked then); src missing -> block on src.
void BlockingMove(ExecContext& ctx, const ArgList& argv, ListEnd from, ListEnd to,
                  std::string_view timeout_arg) {
  int64_t timeout_ms = 0;
  if (Status st = ParseBlockTimeout(timeout_arg, ctx.server.engine().NowMs(), &timeout_ms); !st.ok())
    return ReplyError(ctx, st);
  std::optional<std::string> moved;
  const Status st =
      ctx.server.engine().Write(ctx.session.db, {argv[1], argv[2]}, [&](WriteTxn& txn, int64_t now_ms) {
        return MoveInto(ListMove(txn, ctx.server.oids(), ctx.session.db, argv[1], argv[2], from, to, now_ms),
                        &moved);
      });
  if (!st.ok()) return ReplyError(ctx, st);
  if (moved) {
    SignalReady(ctx, argv[2]);
    return ctx.reply.Bulk(*moved);
  }
  if (ctx.block == nullptr) return ctx.reply.Null();  // a null bulk here (t_list.c:1292), not an array
  ctx.block->Block({encoding::ValueType::kList, {argv[1]}, timeout_ms, {}});
}

void BLMove(ExecContext& ctx, const ArgList& argv) {
  ListEnd from{}, to{};
  if (Status st = ParseListEndArg(argv[3], &from); !st.ok()) return ReplyError(ctx, st);
  if (Status st = ParseListEndArg(argv[4], &to); !st.ok()) return ReplyError(ctx, st);
  BlockingMove(ctx, argv, from, to, argv[5]);
}

void BRPopLPush(ExecContext& ctx, const ArgList& argv) {
  BlockingMove(ctx, argv, ListEnd::kRight, ListEnd::kLeft, argv[3]);
}

}  // namespace

void RegisterBlockingListCommands(CommandRegistry& r) {
  r.Register({"blpop", -3, kCmdWrite, 1, -2, 1, &BLPop});
  r.Register({"brpop", -3, kCmdWrite, 1, -2, 1, &BRPop});
  r.Register({"blmpop", -5, kCmdWrite, 0, 0, 0, &BLMPop});
  r.Register({"blmove", 6, kCmdWrite, 1, 2, 1, &BLMove});
  r.Register({"brpoplpush", 4, kCmdWrite, 1, 2, 1, &BRPopLPush});
}

}  // namespace tideskv
