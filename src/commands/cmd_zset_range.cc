#include <optional>
#include <string>
#include <vector>

#include "commands/all_commands.h"
#include "commands/cmd_util.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "commands/zset_reply.h"
#include "common/string_util.h"
#include "types/zset_query.h"

namespace tideskv {
namespace {

// What a legacy command fixes in advance (nullopt: chosen by the arguments, as for ZRANGE/ZRANGESTORE).
struct ZrangeForm {
  bool store = false;
  std::optional<RangeBy> by;
  std::optional<bool> reverse;
};

Status Fail(std::string_view text) { return Status::UserError(std::string(text)); }

// zrangeGenericCommand (redis72-zset.md §3.1). argv[key_idx] is the source key.
Status ParseZrange(const ArgList& argv, size_t key_idx, const ZrangeForm& form, RangeSpec* spec,
                   bool* with_scores) {
  std::optional<RangeBy> by = form.by;
  std::optional<bool> reverse = form.reverse;
  for (size_t i = key_idx + 3; i < argv.size(); ++i) {
    const size_t left = argv.size() - i - 1;
    if (!form.store && EqualsIgnoreCase(argv[i], "WITHSCORES")) {
      *with_scores = true;
    } else if (EqualsIgnoreCase(argv[i], "LIMIT") && left >= 2) {
      if (!ParseInt64(argv[i + 1], &spec->offset) || !ParseInt64(argv[i + 2], &spec->count))
        return Fail(kErrNotInteger);
      i += 2;
    } else if (!reverse && EqualsIgnoreCase(argv[i], "REV")) {
      reverse = true;
    } else if (!by && EqualsIgnoreCase(argv[i], "BYLEX")) {
      by = RangeBy::kLex;
    } else if (!by && EqualsIgnoreCase(argv[i], "BYSCORE")) {
      by = RangeBy::kScore;
    } else {
      return Fail(kErrSyntax);
    }
  }
  spec->by = by.value_or(RangeBy::kRank);
  spec->reverse = reverse.value_or(false);
  if (spec->count != -1 && spec->by == RangeBy::kRank) return Fail(kErrZrangeLimit);
  if (*with_scores && spec->by == RangeBy::kLex) return Fail(kErrZrangeWithscoresLex);
  std::string_view min = argv[key_idx + 1], max = argv[key_idx + 2];
  if (spec->by != RangeBy::kRank && spec->reverse) std::swap(min, max);  // REV takes max min
  switch (spec->by) {
    case RangeBy::kRank:
      if (!ParseInt64(min, &spec->start) || !ParseInt64(max, &spec->stop)) return Fail(kErrNotInteger);
      return Status::Ok();
    case RangeBy::kScore:
      return ParseScoreRange(min, max, &spec->score);
    case RangeBy::kLex:
      return ParseLexRange(min, max, &spec->lex);
  }
  return Status::Ok();
}

void RangeCommand(ExecContext& ctx, const ArgList& argv, const ZrangeForm& form) {
  RangeSpec spec;
  bool with_scores = false;
  if (Status st = ParseZrange(argv, 1, form, &spec, &with_scores); !st.ok()) return ReplyError(ctx, st);
  std::vector<ScoreMember> items;
  const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) {
    return MoveInto(ZsetRange(view, ctx.session.db, argv[1], spec, now_ms), &items);
  });
  if (ok) ReplyScored(ctx, items, with_scores);
}

void ZRange(ExecContext& ctx, const ArgList& argv) { RangeCommand(ctx, argv, {}); }
void ZRevRange(ExecContext& ctx, const ArgList& argv) {
  RangeCommand(ctx, argv, {false, RangeBy::kRank, true});
}
void ZRangeByScore(ExecContext& ctx, const ArgList& argv) {
  RangeCommand(ctx, argv, {false, RangeBy::kScore, false});
}
void ZRevRangeByScore(ExecContext& ctx, const ArgList& argv) {
  RangeCommand(ctx, argv, {false, RangeBy::kScore, true});
}
void ZRangeByLex(ExecContext& ctx, const ArgList& argv) {
  RangeCommand(ctx, argv, {false, RangeBy::kLex, false});
}
void ZRevRangeByLex(ExecContext& ctx, const ArgList& argv) {
  RangeCommand(ctx, argv, {false, RangeBy::kLex, true});
}

void ZRangeStore(ExecContext& ctx, const ArgList& argv) {
  RangeSpec spec;
  bool with_scores = false;
  if (Status st = ParseZrange(argv, 2, {true, std::nullopt, std::nullopt}, &spec, &with_scores); !st.ok())
    return ReplyError(ctx, st);
  int64_t n = 0;
  const Status st =
      ctx.server.engine().Write(ctx.session.db, {argv[1], argv[2]}, [&](WriteTxn& txn, int64_t now_ms) {
        return MoveInto(
            ZsetRangeStore(txn, ctx.server.oids(), ctx.session.db, argv[1], argv[2], spec, now_ms), &n);
      });
  if (!st.ok()) return ReplyError(ctx, st);
  if (n > 0) SignalReady(ctx, argv[1]);
  ctx.reply.Integer(n);
}

// ZRANK/ZREVRANK key member [WITHSCORE] (Redis 7.2).
void RankCommand(ExecContext& ctx, const ArgList& argv, bool reverse) {
  if (argv.size() > 4) return ctx.reply.Error(ArityError(reverse ? "zrevrank" : "zrank"));
  const bool with_score = argv.size() == 4;
  if (with_score && !EqualsIgnoreCase(argv[3], "WITHSCORE")) return ctx.reply.Error(kErrSyntax);
  std::optional<RankResult> r;
  const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) {
    return MoveInto(ZsetRank(view, ctx.session.db, argv[1], argv[2], reverse, now_ms), &r);
  });
  if (!ok) return;
  if (!r) return with_score ? ctx.reply.NullArray() : ctx.reply.Null();
  if (!with_score) return ctx.reply.Integer(r->rank);
  ctx.reply.ArrayHeader(2);
  ctx.reply.Integer(r->rank);
  ctx.reply.Double(r->score);
}

void ZRank(ExecContext& ctx, const ArgList& argv) { RankCommand(ctx, argv, false); }
void ZRevRank(ExecContext& ctx, const ArgList& argv) { RankCommand(ctx, argv, true); }

}  // namespace

void RegisterZsetRangeCommands(CommandRegistry& r) {
  r.Register({"zrange", -4, kCmdReadOnly, 1, 1, 1, &ZRange});
  r.Register({"zrangestore", -5, kCmdWrite, 1, 2, 1, &ZRangeStore});
  r.Register({"zrevrange", -4, kCmdReadOnly, 1, 1, 1, &ZRevRange});
  r.Register({"zrangebyscore", -4, kCmdReadOnly, 1, 1, 1, &ZRangeByScore});
  r.Register({"zrevrangebyscore", -4, kCmdReadOnly, 1, 1, 1, &ZRevRangeByScore});
  r.Register({"zrangebylex", -4, kCmdReadOnly, 1, 1, 1, &ZRangeByLex});
  r.Register({"zrevrangebylex", -4, kCmdReadOnly, 1, 1, 1, &ZRevRangeByLex});
  r.Register({"zrank", -3, kCmdReadOnly | kCmdFast, 1, 1, 1, &ZRank});
  r.Register({"zrevrank", -3, kCmdReadOnly | kCmdFast, 1, 1, 1, &ZRevRank});
}

}  // namespace tideskv
