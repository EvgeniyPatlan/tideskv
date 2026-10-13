#include <optional>
#include <string>
#include <vector>

#include "commands/all_commands.h"
#include "commands/cmd_util.h"
#include "commands/command_rng.h"
#include "commands/count_args.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "types/set_type.h"

namespace tideskv {
namespace {

void ReplySet(ExecContext& ctx, const std::vector<std::string>& members) {
  ctx.reply.SetHeader(members.size());  // RESP3 '~', RESP2 array
  for (const std::string& m : members) ctx.reply.Bulk(m);
}

void SAdd(ExecContext& ctx, const ArgList& argv) {
  const auto members = ArgsFrom(argv, 2);
  int64_t n = 0;
  const bool ok = WriteOneKey(ctx, argv[1], [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(SetAdd(txn, ctx.server.oids(), ctx.session.db, argv[1], members, now_ms), &n);
  });
  if (ok) ctx.reply.Integer(n);
}

void SRem(ExecContext& ctx, const ArgList& argv) {
  const auto members = ArgsFrom(argv, 2);
  int64_t n = 0;
  const bool ok = WriteOneKey(ctx, argv[1], [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(SetRemove(txn, ctx.session.db, argv[1], members, now_ms), &n);
  });
  if (ok) ctx.reply.Integer(n);
}

void SCard(ExecContext& ctx, const ArgList& argv) {
  int64_t n = 0;
  const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) {
    return MoveInto(SetCard(view, ctx.session.db, argv[1], now_ms), &n);
  });
  if (ok) ctx.reply.Integer(n);
}

void SIsMember(ExecContext& ctx, const ArgList& argv) {
  bool member = false;
  const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) {
    return MoveInto(SetIsMember(view, ctx.session.db, argv[1], argv[2], now_ms), &member);
  });
  if (ok) ctx.reply.Integer(member ? 1 : 0);
}

void SMIsMember(ExecContext& ctx, const ArgList& argv) {
  const auto members = ArgsFrom(argv, 2);
  std::vector<int64_t> flags;
  const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) {
    return MoveInto(SetMultiIsMember(view, ctx.session.db, argv[1], members, now_ms), &flags);
  });
  if (!ok) return;
  ctx.reply.ArrayHeader(flags.size());
  for (int64_t f : flags) ctx.reply.Integer(f);
}

void SMembers(ExecContext& ctx, const ArgList& argv) {
  std::vector<std::string> members;
  const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) {
    return MoveInto(SetMembers(view, ctx.session.db, argv[1], now_ms), &members);
  });
  if (ok) ReplySet(ctx, members);
}

// Redis 7.2 spopCommand: SPOP key | SPOP key count (count >= 0, else kErrMustBePositive).
void SPop(ExecContext& ctx, const ArgList& argv) {
  if (argv.size() > 3) return ctx.reply.Error(kErrSyntax);
  if (argv.size() == 2) {
    std::optional<std::string> member;
    const bool ok = WriteOneKey(ctx, argv[1], [&](WriteTxn& txn, int64_t now_ms) {
      return MoveInto(SetPopOne(txn, ctx.session.db, argv[1], CommandRng(), now_ms), &member);
    });
    if (ok) ReplyOptional(ctx, member);
    return;
  }
  int64_t count = 0;
  if (Status st = ParsePositiveCount(argv[2], kErrMustBePositive, &count); !st.ok())
    return ReplyError(ctx, st);
  std::vector<std::string> members;
  const bool ok = WriteOneKey(ctx, argv[1], [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(SetPop(txn, ctx.session.db, argv[1], static_cast<uint64_t>(count), CommandRng(), now_ms),
                    &members);
  });
  if (ok) ReplySet(ctx, members);
}

// Redis 7.2 srandmemberCommand: an array (never a set) for the count form.
void SRandMember(ExecContext& ctx, const ArgList& argv) {
  if (argv.size() > 3) return ctx.reply.Error(kErrSyntax);
  if (argv.size() == 2) {
    std::optional<std::string> member;
    const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) {
      return MoveInto(SetRandomMember(view, ctx.session.db, argv[1], CommandRng(), now_ms), &member);
    });
    if (ok) ReplyOptional(ctx, member);
    return;
  }
  int64_t count = 0;
  if (Status st = ParseRandomCount(argv[2], &count); !st.ok()) return ReplyError(ctx, st);
  std::vector<std::string> members;
  const bool ok = RunRead(ctx, [&](ReadView& view, int64_t now_ms) {
    return MoveInto(SetRandomMembers(view, ctx.session.db, argv[1], count, CommandRng(), now_ms), &members);
  });
  if (!ok) return;
  ctx.reply.ArrayHeader(members.size());
  for (const std::string& m : members) ctx.reply.Bulk(m);
}

void SMove(ExecContext& ctx, const ArgList& argv) {
  int64_t moved = 0;
  // Redis signals the destination whenever the member left the source (t_set.c smoveCommand).
  std::vector<std::string> also;
  const Status st = ctx.server.engine().Write(
      ctx.session.db, {argv[1], argv[2]},
      [&](WriteTxn& txn, int64_t now_ms) {
        Status s = MoveInto(
            SetMove(txn, ctx.server.oids(), ctx.session.db, argv[1], argv[2], argv[3], now_ms), &moved);
        if (s.ok() && moved == 1 && argv[1] != argv[2]) also.push_back(argv[2]);
        return s;
      },
      &also);
  if (!st.ok()) return ReplyError(ctx, st);
  ctx.reply.Integer(moved);
}

}  // namespace

void RegisterSetCommands(CommandRegistry& r) {
  r.Register({"sadd", -3, kCmdWrite | kCmdFast, 1, 1, 1, &SAdd});
  r.Register({"srem", -3, kCmdWrite | kCmdFast, 1, 1, 1, &SRem});
  r.Register({"scard", 2, kCmdReadOnly | kCmdFast, 1, 1, 1, &SCard});
  r.Register({"sismember", 3, kCmdReadOnly | kCmdFast, 1, 1, 1, &SIsMember});
  r.Register({"smismember", -3, kCmdReadOnly | kCmdFast, 1, 1, 1, &SMIsMember});
  r.Register({"smembers", 2, kCmdReadOnly, 1, 1, 1, &SMembers});
  r.Register({"spop", -2, kCmdWrite | kCmdFast, 1, 1, 1, &SPop});
  r.Register({"srandmember", -2, kCmdReadOnly, 1, 1, 1, &SRandMember});
  r.Register({"smove", 4, kCmdWrite | kCmdFast, 1, 2, 1, &SMove});
}

}  // namespace tideskv
