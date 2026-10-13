#include <optional>
#include <string>
#include <vector>

#include "commands/all_commands.h"
#include "commands/cmd_util.h"
#include "commands/command_rng.h"
#include "commands/count_args.h"
#include "commands/numkeys_args.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "commands/zset_reply.h"
#include "common/string_util.h"
#include "types/zset_pop.h"

namespace tideskv {
namespace {

void RemoveRange(ExecContext& ctx, const ArgList& argv, const RangeSpec& spec) {
  int64_t n = 0;
  const bool ok = WriteOneKey(ctx, argv[1], [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(ZsetRemoveRange(txn, ctx.session.db, argv[1], spec, now_ms), &n);
  });
  if (ok) ctx.reply.Integer(n);
}

void ZRemRangeByRank(ExecContext& ctx, const ArgList& argv) {
  RangeSpec spec;
  if (!ParseInt64(argv[2], &spec.start) || !ParseInt64(argv[3], &spec.stop))
    return ctx.reply.Error(kErrNotInteger);
  RemoveRange(ctx, argv, spec);
}

void ZRemRangeByScore(ExecContext& ctx, const ArgList& argv) {
  RangeSpec spec;
  spec.by = RangeBy::kScore;
  if (Status st = ParseScoreRange(argv[2], argv[3], &spec.score); !st.ok()) return ReplyError(ctx, st);
  RemoveRange(ctx, argv, spec);
}

void ZRemRangeByLex(ExecContext& ctx, const ArgList& argv) {
  RangeSpec spec;
  spec.by = RangeBy::kLex;
  if (Status st = ParseLexRange(argv[2], argv[3], &spec.lex); !st.ok()) return ReplyError(ctx, st);
  RemoveRange(ctx, argv, spec);
}

// genericZpopCommand: [count] via getPositiveLong (any bad value: kErrMustBePositive).
void PopCommand(ExecContext& ctx, const ArgList& argv, bool max) {
  if (argv.size() > 3) return ctx.reply.Error(kErrSyntax);
  const bool has_count = argv.size() == 3;
  int64_t count = 1;
  if (has_count) {
    if (Status st = ParsePositiveCount(argv[2], kErrMustBePositive, &count); !st.ok())
      return ReplyError(ctx, st);
  }
  std::optional<std::vector<ScoreMember>> popped;
  const bool ok = WriteOneKey(ctx, argv[1], [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(ZsetPop(txn, ctx.session.db, argv[1], max, static_cast<uint64_t>(count), now_ms),
                    &popped);
  });
  if (!ok) return;
  const std::vector<ScoreMember> items = popped.value_or(std::vector<ScoreMember>{});
  if (has_count || ctx.reply.protocol() < 3 || items.empty()) return ReplyScored(ctx, items, true);
  ctx.reply.ArrayHeader(2);  // RESP3 without a count: one flat pair
  ctx.reply.Bulk(items.front().member);
  ctx.reply.Double(items.front().score);
}

void ZPopMin(ExecContext& ctx, const ArgList& argv) { PopCommand(ctx, argv, false); }
void ZPopMax(ExecContext& ctx, const ArgList& argv) { PopCommand(ctx, argv, true); }

void ZMPop(ExecContext& ctx, const ArgList& argv) {
  MultiPopArgs a;
  if (Status st = ParseMultiPop(argv, 1, "MIN", "MAX", &a); !st.ok()) return ReplyError(ctx, st);
  const auto keys = ArgsFrom(argv, a.first_key, a.first_key + a.num_keys);
  std::optional<ZPoppedFrom> popped;
  const Status st = ctx.server.engine().Write(ctx.session.db, keys, [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(ZsetPopFirst(txn, ctx.session.db, keys, a.second, static_cast<uint64_t>(a.count), now_ms),
                    &popped);
  });
  if (!st.ok()) return ReplyError(ctx, st);
  ReplyZPopped(ctx, popped);
}

// zrandmemberCommand: count first, then the WITHSCORES token, then the WITHSCORES range limit.
void ZRandMember(ExecContext& ctx, const ArgList& argv) {
  if (argv.size() == 2) {
    std::optional<std::string> m;
    const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) {
      return MoveInto(ZsetRandomMember(view, ctx.session.db, argv[1], CommandRng(), now_ms), &m);
    });
    if (ok) ReplyOptional(ctx, m);
    return;
  }
  int64_t count = 0;
  if (Status st = ParseRandomCount(argv[2], &count); !st.ok()) return ReplyError(ctx, st);
  if (argv.size() > 4 || (argv.size() == 4 && !EqualsIgnoreCase(argv[3], "WITHSCORES")))
    return ctx.reply.Error(kErrSyntax);
  const bool with_scores = argv.size() == 4;
  constexpr int64_t kHalf = INT64_MAX / 2;
  if (with_scores && (count < -kHalf || count > kHalf)) return ctx.reply.Error(kErrValueOutOfRange);
  std::vector<ScoreMember> items;
  const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) {
    return MoveInto(ZsetRandomMembers(view, ctx.session.db, argv[1], count, CommandRng(), now_ms), &items);
  });
  if (ok) ReplyScored(ctx, items, with_scores);
}

}  // namespace

void RegisterZsetPopCommands(CommandRegistry& r) {
  r.Register({"zremrangebyrank", 4, kCmdWrite, 1, 1, 1, &ZRemRangeByRank});
  r.Register({"zremrangebyscore", 4, kCmdWrite, 1, 1, 1, &ZRemRangeByScore});
  r.Register({"zremrangebylex", 4, kCmdWrite, 1, 1, 1, &ZRemRangeByLex});
  r.Register({"zpopmin", -2, kCmdWrite | kCmdFast, 1, 1, 1, &ZPopMin});
  r.Register({"zpopmax", -2, kCmdWrite | kCmdFast, 1, 1, 1, &ZPopMax});
  r.Register({"zmpop", -4, kCmdWrite, 0, 0, 0, &ZMPop});
  r.Register({"zrandmember", -2, kCmdReadOnly, 1, 1, 1, &ZRandMember});
}

}  // namespace tideskv
