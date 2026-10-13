#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "commands/all_commands.h"
#include "commands/cmd_util.h"
#include "commands/count_args.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "commands/zset_reply.h"
#include "common/score_format.h"
#include "common/string_util.h"
#include "types/zset_algebra.h"
#include "types/zset_query.h"

namespace tideskv {
namespace {

struct AlgebraArgs {
  size_t first_key = 0, num_keys = 0;
  std::vector<double> weights;
  Aggregate aggregate = Aggregate::kSum;
  bool with_scores = false;
  int64_t limit = 0;
};

Status Fail(std::string_view text) { return Status::UserError(std::string(text)); }

// numkeys and the argument count (run before any key is read).
Status ParseNumkeys(const ArgList& argv, size_t numkeys_idx, std::string_view name, AlgebraArgs* a) {
  int64_t n = 0;
  if (!ParseInt64(argv[numkeys_idx], &n)) return Fail(kErrNotInteger);
  if (n < 1) return Status::UserError(AtLeastOneKeyError(name));
  if (static_cast<uint64_t>(n) > argv.size() - numkeys_idx - 1) return Fail(kErrSyntax);
  a->first_key = numkeys_idx + 1;
  a->num_keys = static_cast<size_t>(n);
  a->weights.assign(a->num_keys, 1.0);
  return Status::Ok();
}

// The options after the keys (run after the keys were type-checked, as in Redis).
Status ParseOptions(const ArgList& argv, ZsetOp op, bool store, bool card_only, AlgebraArgs* a) {
  for (size_t j = a->first_key + a->num_keys; j < argv.size(); ++j) {
    const size_t remaining = argv.size() - j;
    const bool weighted = op != ZsetOp::kDiff && !card_only;
    if (weighted && remaining >= a->num_keys + 1 && EqualsIgnoreCase(argv[j], "WEIGHTS")) {
      for (size_t k = 0; k < a->num_keys; ++k) {
        if (!ParseScore(argv[j + 1 + k], &a->weights[k])) return Fail(kErrWeightNotFloat);
      }
      j += a->num_keys;
    } else if (weighted && remaining >= 2 && EqualsIgnoreCase(argv[j], "AGGREGATE")) {
      const std::string agg = ToLower(argv[j + 1]);
      if (agg == "sum") {
        a->aggregate = Aggregate::kSum;
      } else if (agg == "min") {
        a->aggregate = Aggregate::kMin;
      } else if (agg == "max") {
        a->aggregate = Aggregate::kMax;
      } else {
        return Fail(kErrSyntax);
      }
      ++j;
    } else if (!store && !card_only && EqualsIgnoreCase(argv[j], "WITHSCORES")) {
      a->with_scores = true;
    } else if (card_only && remaining >= 2 && EqualsIgnoreCase(argv[j], "LIMIT")) {
      if (Status st = ParsePositiveCount(argv[j + 1], kErrLimitNegative, &a->limit); !st.ok()) return st;
      ++j;
    } else {
      return Fail(kErrSyntax);
    }
  }
  return Status::Ok();
}

bool FirstKeyRepeated(const std::vector<std::string_view>& keys) {
  return std::find(keys.begin() + 1, keys.end(), keys.front()) != keys.end();
}

void Combine(ExecContext& ctx, const ArgList& argv, ZsetOp op, std::string_view name) {
  AlgebraArgs a;
  if (Status st = ParseNumkeys(argv, 1, name, &a); !st.ok()) return ReplyError(ctx, st);
  const auto keys = ArgsFrom(argv, a.first_key, a.first_key + a.num_keys);
  std::vector<ScoreMember> items;
  const bool ok = RunSnapshot(ctx, [&](ReadView& view, int64_t now_ms) -> Status {
    auto sources = LoadZsetSources(view, ctx.session.db, keys, now_ms);
    if (!sources.ok()) return sources.status();
    if (Status st = ParseOptions(argv, op, false, false, &a); !st.ok()) return st;
    items = CombineZsets(op, *sources, a.weights, a.aggregate, FirstKeyRepeated(keys));
    return Status::Ok();
  });
  if (ok) ReplyScored(ctx, items, a.with_scores);
}

void CombineStore(ExecContext& ctx, const ArgList& argv, ZsetOp op, std::string_view name) {
  AlgebraArgs a;
  if (Status st = ParseNumkeys(argv, 2, name, &a); !st.ok()) return ReplyError(ctx, st);
  const auto keys = ArgsFrom(argv, a.first_key, a.first_key + a.num_keys);
  const auto locked = ArgsFrom(argv, 1, a.first_key + a.num_keys);  // dst and every source
  int64_t stored = 0;
  const Status st =
      ctx.server.engine().Write(ctx.session.db, locked, [&](WriteTxn& txn, int64_t now_ms) -> Status {
        auto sources = LoadZsetSources(txn, ctx.session.db, keys, now_ms);
        if (!sources.ok()) return sources.status();
        if (Status pst = ParseOptions(argv, op, true, false, &a); !pst.ok()) return pst;
        const auto items = CombineZsets(op, *sources, a.weights, a.aggregate, FirstKeyRepeated(keys));
        stored = static_cast<int64_t>(items.size());
        return StoreZset(txn, ctx.server.oids(), ctx.session.db, argv[1], items, now_ms);
      });
  if (!st.ok()) return ReplyError(ctx, st);
  if (stored > 0) SignalReady(ctx, argv[1]);
  ctx.reply.Integer(stored);
}

void ZUnion(ExecContext& ctx, const ArgList& argv) { Combine(ctx, argv, ZsetOp::kUnion, "zunion"); }
void ZInter(ExecContext& ctx, const ArgList& argv) { Combine(ctx, argv, ZsetOp::kInter, "zinter"); }
void ZDiff(ExecContext& ctx, const ArgList& argv) { Combine(ctx, argv, ZsetOp::kDiff, "zdiff"); }
void ZUnionStore(ExecContext& ctx, const ArgList& argv) {
  CombineStore(ctx, argv, ZsetOp::kUnion, "zunionstore");
}
void ZInterStore(ExecContext& ctx, const ArgList& argv) {
  CombineStore(ctx, argv, ZsetOp::kInter, "zinterstore");
}
void ZDiffStore(ExecContext& ctx, const ArgList& argv) {
  CombineStore(ctx, argv, ZsetOp::kDiff, "zdiffstore");
}

void ZInterCard(ExecContext& ctx, const ArgList& argv) {
  AlgebraArgs a;
  if (Status st = ParseNumkeys(argv, 1, "zintercard", &a); !st.ok()) return ReplyError(ctx, st);
  const auto keys = ArgsFrom(argv, a.first_key, a.first_key + a.num_keys);
  uint64_t n = 0;
  const bool ok = RunSnapshot(ctx, [&](ReadView& view, int64_t now_ms) -> Status {
    auto sources = LoadZsetSources(view, ctx.session.db, keys, now_ms);
    if (!sources.ok()) return sources.status();
    if (Status st = ParseOptions(argv, ZsetOp::kInter, false, true, &a); !st.ok()) return st;
    n = ZsetInterCardinality(*sources, static_cast<uint64_t>(a.limit));
    return Status::Ok();
  });
  if (ok) ctx.reply.Integer(static_cast<int64_t>(n));
}

}  // namespace

void RegisterZsetAlgebraCommands(CommandRegistry& r) {
  r.Register({"zunion", -3, kCmdReadOnly, 0, 0, 0, &ZUnion});
  r.Register({"zinter", -3, kCmdReadOnly, 0, 0, 0, &ZInter});
  r.Register({"zdiff", -3, kCmdReadOnly, 0, 0, 0, &ZDiff});
  r.Register({"zunionstore", -4, kCmdWrite, 1, 1, 1, &ZUnionStore});
  r.Register({"zinterstore", -4, kCmdWrite, 1, 1, 1, &ZInterStore});
  r.Register({"zdiffstore", -4, kCmdWrite, 1, 1, 1, &ZDiffStore});
  r.Register({"zintercard", -3, kCmdReadOnly, 0, 0, 0, &ZInterCard});
}

}  // namespace tideskv
