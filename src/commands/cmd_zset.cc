#include <optional>
#include <string>
#include <vector>

#include "commands/all_commands.h"
#include "commands/cmd_util.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "common/score_format.h"
#include "common/string_util.h"
#include "types/zset_type.h"

namespace tideskv {
namespace {

void ReplyScore(ExecContext& ctx, const std::optional<double>& s) {
  s ? ctx.reply.Double(*s) : ctx.reply.Null();
}

// zaddGenericCommand (redis72-zset.md §1): options, pair count, flag conflicts, every score, then the key.
Status ParseZadd(const ArgList& argv, bool incr, ZAddFlags* f, std::vector<ScoreMember>* pairs) {
  f->incr = incr;
  size_t i = 2;
  for (; i < argv.size(); ++i) {
    const std::string opt = ToLower(argv[i]);
    if (opt == "nx") {
      f->nx = true;
    } else if (opt == "xx") {
      f->xx = true;
    } else if (opt == "ch") {
      f->ch = true;
    } else if (opt == "incr") {
      f->incr = true;
    } else if (opt == "gt") {
      f->gt = true;
    } else if (opt == "lt") {
      f->lt = true;
    } else {
      break;
    }
  }
  const size_t elements = argv.size() - i;
  if (elements == 0 || elements % 2 != 0) return Status::UserError(std::string(kErrSyntax));
  if (f->nx && f->xx) return Status::UserError(std::string(kErrZaddXxNx));
  if ((f->gt && f->nx) || (f->lt && f->nx) || (f->gt && f->lt))
    return Status::UserError(std::string(kErrZaddGtLtNx));
  if (f->incr && elements > 2) return Status::UserError(std::string(kErrZaddIncrPair));
  for (; i < argv.size(); i += 2) {
    ScoreMember p;
    if (!ParseScore(argv[i], &p.score)) return Status::UserError(std::string(kErrNotFloat));
    p.member = argv[i + 1];
    pairs->push_back(std::move(p));
  }
  return Status::Ok();
}

void ZaddGeneric(ExecContext& ctx, const ArgList& argv, bool incr) {
  ZAddFlags f;
  std::vector<ScoreMember> pairs;
  if (Status st = ParseZadd(argv, incr, &f, &pairs); !st.ok()) return ReplyError(ctx, st);
  ZAddResult r;
  const bool ok = WriteOneKey(ctx, argv[1], [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(ZsetAdd(txn, ctx.server.oids(), ctx.session.db, argv[1], f, pairs, now_ms), &r);
  });
  if (!ok) return;
  if (r.added > 0) SignalReady(ctx, argv[1]);
  if (f.incr) return ReplyScore(ctx, r.incr_score);
  ctx.reply.Integer(f.ch ? r.added + r.updated : r.added);
}

void ZAdd(ExecContext& ctx, const ArgList& argv) { ZaddGeneric(ctx, argv, false); }
void ZIncrBy(ExecContext& ctx, const ArgList& argv) { ZaddGeneric(ctx, argv, true); }

void ZRem(ExecContext& ctx, const ArgList& argv) {
  const auto members = ArgsFrom(argv, 2);
  int64_t n = 0;
  const bool ok = WriteOneKey(ctx, argv[1], [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(ZsetRemove(txn, ctx.session.db, argv[1], members, now_ms), &n);
  });
  if (ok) ctx.reply.Integer(n);
}

void ZCard(ExecContext& ctx, const ArgList& argv) {
  int64_t n = 0;
  const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) {
    return MoveInto(ZsetCard(view, ctx.session.db, argv[1], now_ms), &n);
  });
  if (ok) ctx.reply.Integer(n);
}

void ZScore(ExecContext& ctx, const ArgList& argv) {
  std::optional<double> s;
  const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) {
    return MoveInto(ZsetScore(view, ctx.session.db, argv[1], argv[2], now_ms), &s);
  });
  if (ok) ReplyScore(ctx, s);
}

void ZMScore(ExecContext& ctx, const ArgList& argv) {
  const auto members = ArgsFrom(argv, 2);
  std::vector<std::optional<double>> scores;
  const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) {
    return MoveInto(ZsetMultiScore(view, ctx.session.db, argv[1], members, now_ms), &scores);
  });
  if (!ok) return;
  ctx.reply.ArrayHeader(scores.size());
  for (const auto& s : scores) ReplyScore(ctx, s);
}

// The range is parsed before the key lookup (bad range beats WRONGTYPE and a missing key).
void ZCount(ExecContext& ctx, const ArgList& argv) {
  ScoreRange range;
  if (Status st = ParseScoreRange(argv[2], argv[3], &range); !st.ok()) return ReplyError(ctx, st);
  int64_t n = 0;
  const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) {
    return MoveInto(ZsetCount(view, ctx.session.db, argv[1], range, now_ms), &n);
  });
  if (ok) ctx.reply.Integer(n);
}

void ZLexCount(ExecContext& ctx, const ArgList& argv) {
  LexRange range;
  if (Status st = ParseLexRange(argv[2], argv[3], &range); !st.ok()) return ReplyError(ctx, st);
  int64_t n = 0;
  const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) {
    return MoveInto(ZsetLexCount(view, ctx.session.db, argv[1], range, now_ms), &n);
  });
  if (ok) ctx.reply.Integer(n);
}

}  // namespace

void RegisterZsetCommands(CommandRegistry& r) {
  r.Register({"zadd", -4, kCmdWrite | kCmdFast, 1, 1, 1, &ZAdd});
  r.Register({"zincrby", 4, kCmdWrite | kCmdFast, 1, 1, 1, &ZIncrBy});
  r.Register({"zrem", -3, kCmdWrite | kCmdFast, 1, 1, 1, &ZRem});
  r.Register({"zcard", 2, kCmdReadOnly | kCmdFast, 1, 1, 1, &ZCard});
  r.Register({"zscore", 3, kCmdReadOnly | kCmdFast, 1, 1, 1, &ZScore});
  r.Register({"zmscore", -3, kCmdReadOnly | kCmdFast, 1, 1, 1, &ZMScore});
  r.Register({"zcount", 4, kCmdReadOnly | kCmdFast, 1, 1, 1, &ZCount});
  r.Register({"zlexcount", 4, kCmdReadOnly | kCmdFast, 1, 1, 1, &ZLexCount});
}

}  // namespace tideskv
