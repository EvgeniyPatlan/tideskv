#include <optional>
#include <string>
#include <vector>

#include "commands/all_commands.h"
#include "commands/block_args.h"
#include "commands/cmd_util.h"
#include "commands/numkeys_args.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "commands/zset_reply.h"
#include "types/zset_pop.h"

namespace tideskv {
namespace {

void BlockingZPop(ExecContext& ctx, const std::vector<std::string_view>& keys, bool max, uint64_t count,
                  int64_t timeout_ms, bool nested_reply) {
  std::optional<ZPoppedFrom> popped;
  const Status st = ctx.server.engine().Write(ctx.session.db, keys, [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(ZsetPopFirst(txn, ctx.session.db, keys, max, count, now_ms), &popped);
  });
  if (!st.ok()) return ReplyError(ctx, st);
  if (popped) {
    if (nested_reply) return ReplyZPopped(ctx, popped);  // BZMPOP
    ctx.reply.ArrayHeader(3);                            // BZPOPMIN/MAX: [key, member, score]
    ctx.reply.Bulk(popped->key);
    ctx.reply.Bulk(popped->items.front().member);
    return ctx.reply.Double(popped->items.front().score);
  }
  if (ctx.block == nullptr) return ctx.reply.NullArray();
  ctx.block->Block(
      {encoding::ValueType::kZSet, std::vector<std::string>(keys.begin(), keys.end()), timeout_ms});
}

void BZPopCommand(ExecContext& ctx, const ArgList& argv, bool max) {
  int64_t timeout_ms = 0;
  if (Status st = ParseBlockTimeout(argv.back(), ctx.server.engine().NowMs(), &timeout_ms); !st.ok())
    return ReplyError(ctx, st);
  BlockingZPop(ctx, ArgsFrom(argv, 1, argv.size() - 1), max, 1, timeout_ms, false);
}

void BZPopMin(ExecContext& ctx, const ArgList& argv) { BZPopCommand(ctx, argv, false); }
void BZPopMax(ExecContext& ctx, const ArgList& argv) { BZPopCommand(ctx, argv, true); }

void BZMPop(ExecContext& ctx, const ArgList& argv) {
  MultiPopArgs a;
  if (Status st = ParseMultiPop(argv, 2, "MIN", "MAX", &a); !st.ok()) return ReplyError(ctx, st);
  int64_t timeout_ms = 0;
  if (Status st = ParseBlockTimeout(argv[1], ctx.server.engine().NowMs(), &timeout_ms); !st.ok())
    return ReplyError(ctx, st);
  BlockingZPop(ctx, ArgsFrom(argv, a.first_key, a.first_key + a.num_keys), a.second,
               static_cast<uint64_t>(a.count), timeout_ms, true);
}

}  // namespace

void RegisterBlockingZsetCommands(CommandRegistry& r) {
  r.Register({"bzpopmin", -3, kCmdWrite | kCmdFast, 1, -2, 1, &BZPopMin});
  r.Register({"bzpopmax", -3, kCmdWrite | kCmdFast, 1, -2, 1, &BZPopMax});
  r.Register({"bzmpop", -5, kCmdWrite, 0, 0, 0, &BZMPop});
}

}  // namespace tideskv
