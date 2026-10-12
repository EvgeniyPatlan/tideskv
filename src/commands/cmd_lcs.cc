#include <string>
#include <utility>

#include "commands/all_commands.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "common/string_util.h"
#include "exec/engine.h"
#include "types/lcs.h"
#include "types/object_store.h"

namespace tideskv {
namespace {

struct LcsArgs {
  bool len = false, idx = false, with_match_len = false;
  int64_t min_match_len = 0;
};

Result<LcsArgs> ParseLcsArgs(const ArgList& argv) {
  LcsArgs a;
  for (size_t j = 3; j < argv.size(); ++j) {
    const bool more = j + 1 < argv.size();
    if (EqualsIgnoreCase(argv[j], "IDX")) {
      a.idx = true;
    } else if (EqualsIgnoreCase(argv[j], "LEN")) {
      a.len = true;
    } else if (EqualsIgnoreCase(argv[j], "WITHMATCHLEN")) {
      a.with_match_len = true;
    } else if (EqualsIgnoreCase(argv[j], "MINMATCHLEN") && more) {
      if (!ParseInt64(argv[++j], &a.min_match_len)) return Status::UserError(std::string(kErrNotInteger));
    } else {
      return Status::UserError(std::string(kErrSyntax));
    }
  }
  if (a.idx && a.len) return Status::UserError(std::string(kErrLcsLenAndIdx));
  return a;
}

// Reads both values on one snapshot; a missing key is "".
Status LoadPair(ExecContext& ctx, const ArgList& argv, std::string* a, std::string* b) {
  Engine& e = ctx.server.engine();
  return e.Snapshot([&](ReadView& v, int64_t now_ms) {
    for (auto [key, out] : {std::pair{&argv[1], a}, std::pair{&argv[2], b}}) {
      auto obj = LoadObject(v, ctx.session.db, *key, now_ms);
      if (!obj.ok()) return obj.status();
      if (obj->live() && !obj->IsString()) {
        return Status::UserError(std::string(kErrLcsNotStrings));
      }
      *out = obj->live() ? std::move(obj->payload) : std::string();
    }
    return Status::Ok();
  });
}

void ReplyIdx(ExecContext& ctx, const LcsResult& r, bool with_len) {
  resp::ReplyBuilder& out = ctx.reply;
  out.MapHeader(2);
  out.Bulk("matches");
  out.ArrayHeader(r.matches.size());
  for (const LcsMatch& m : r.matches) {
    out.ArrayHeader(with_len ? 3 : 2);
    out.ArrayHeader(2);
    out.Integer(m.a_start);
    out.Integer(m.a_end);
    out.ArrayHeader(2);
    out.Integer(m.b_start);
    out.Integer(m.b_end);
    if (with_len) out.Integer(m.len);
  }
  out.Bulk("len");
  out.Integer(r.length);
}

void Lcs(ExecContext& ctx, const ArgList& argv) {
  std::string a, b;
  if (Status st = LoadPair(ctx, argv, &a, &b); !st.ok()) return ReplyError(ctx, st);
  auto args = ParseLcsArgs(argv);
  if (!args.ok()) return ReplyError(ctx, args.status());
  auto r = ComputeLcs(a, b, args->idx || !args->len, args->min_match_len);
  if (!r.ok()) return ReplyError(ctx, r.status());
  if (args->idx) return ReplyIdx(ctx, *r, args->with_match_len);
  if (args->len) return ctx.reply.Integer(r->length);
  ctx.reply.Bulk(r->sequence);
}

}  // namespace

void RegisterLcsCommand(CommandRegistry& r) { r.Register({"lcs", -3, kCmdReadOnly, 1, 2, 1, &Lcs}); }

}  // namespace tideskv
