#include <string>
#include <vector>

#include "commands/all_commands.h"
#include "commands/cmd_util.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "types/set_type.h"

namespace tideskv {
namespace {

std::vector<std::string_view> ArgsFrom(const ArgList& argv, size_t first) {
  return std::vector<std::string_view>(argv.begin() + static_cast<std::ptrdiff_t>(first), argv.end());
}

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

}  // namespace

void RegisterSetCommands(CommandRegistry& r) {
  r.Register({"sadd", -3, kCmdWrite | kCmdFast, 1, 1, 1, &SAdd});
  r.Register({"srem", -3, kCmdWrite | kCmdFast, 1, 1, 1, &SRem});
  r.Register({"scard", 2, kCmdReadOnly | kCmdFast, 1, 1, 1, &SCard});
  r.Register({"sismember", 3, kCmdReadOnly | kCmdFast, 1, 1, 1, &SIsMember});
  r.Register({"smismember", -3, kCmdReadOnly | kCmdFast, 1, 1, 1, &SMIsMember});
  r.Register({"smembers", 2, kCmdReadOnly, 1, 1, 1, &SMembers});
}

}  // namespace tideskv
