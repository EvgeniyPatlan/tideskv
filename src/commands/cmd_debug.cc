#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "cluster/nodes_parser.h"
#include "commands/all_commands.h"
#include "commands/cmd_util.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "common/string_util.h"
#include "exec/engine.h"
#include "types/digest.h"
#include "types/index_digest.h"
#include "types/special_key_filter.h"

// DEBUG: only the DIGEST / DIGEST-VALUE subcommands (debug.c), with Redis's exact algorithm, plus the
// TidesKV-only DIGEST-INDEX (M7a D23) and SPECIAL-KEYS (B1).
namespace tideskv {
namespace {

void DebugHelp(ExecContext& ctx) {
  static constexpr std::array<std::string_view, 12> kLines = {
      "DIGEST",
      "    Output a hex signature representing the current DB content.",
      "DIGEST-VALUE <key> [<key> ...]",
      "    Output a hex signature of the values of all the specified keys.",
      "DIGEST-INDEX",
      "    Output a hex signature of the TTLs and the expire index, then its entry, missing and orphan "
      "counts.",
      "DIGEST-SLOTS <slot|from-to> [TTL]",
      "    Output a hex signature of the keys in the slot range (TTL: with their exact deadlines).",
      "SPECIAL-KEYS",
      "    Output the blind-SET filter state, then its special and missing key counts.",
      "HELP",
      "    Print this help."};
  ctx.reply.ArrayHeader(kLines.size() + 1);
  ctx.reply.SimpleString("DEBUG <subcommand> [<arg> [value] [opt] ...]. Subcommands are:");
  for (std::string_view line : kLines) ctx.reply.SimpleString(line);
}

void DebugDigest(ExecContext& ctx) {
  Digest20 d{};
  const Engine::LoadingReadScope during_load;  // CF6: DEBUG works while a full sync loads
  const Status st =
      ctx.server.engine().Snapshot([&d](ReadView& v, int64_t) { return MoveInto(DatasetDigest(v), &d); });
  if (!st.ok()) return ReplyError(ctx, st);
  ctx.reply.SimpleString(DigestHex(d));
}

// Each key from a raw read of its meta record: logically expired keys are included, as Redis's dictFind.
void DebugDigestValue(ExecContext& ctx, const ArgList& argv) {
  std::vector<Digest20> digests(argv.size() - 2);
  const Engine::LoadingReadScope during_load;  // CF6
  const uint8_t db = ctx.session.db;
  const Status st = ctx.server.engine().Snapshot([&](ReadView& v, int64_t) {
    for (size_t i = 2; i < argv.size(); ++i) {
      if (Status s = MoveInto(ValueDigest(v, db, argv[i]), &digests[i - 2]); !s.ok()) return s;
    }
    return Status::Ok();
  });
  if (!st.ok()) return ReplyError(ctx, st);
  ctx.reply.ArrayHeader(digests.size());
  for (const auto& d : digests) ctx.reply.SimpleString(DigestHex(d));
}

// M7a D23: TidesKV only. DEBUG DIGEST stays byte-identical to Redis (R6).
void DebugDigestIndex(ExecContext& ctx) {
  IndexDigest d;
  const Engine::LoadingReadScope during_load;  // CF6: DEBUG works while a full sync loads
  const Status st = ctx.server.engine().Snapshot(
      [&d](ReadView& v, int64_t) { return MoveInto(ComputeIndexDigest(v), &d); });
  if (!st.ok()) return ReplyError(ctx, st);
  ctx.reply.ArrayHeader(4);
  ctx.reply.SimpleString(DigestHex(d.digest));
  ctx.reply.Integer(static_cast<int64_t>(d.entries));
  ctx.reply.Integer(static_cast<int64_t>(d.missing));
  ctx.reply.Integer(static_cast<int64_t>(d.orphans));
}

// B1: TidesKV only. Every meta record that is a collection or has a TTL, and how many of them the
// special-key filter lacks (any is a bug: a blind SET could then overwrite a live collection or TTL).
void DebugSpecialKeys(ExecContext& ctx) {
  SpecialKeyCheck c;
  const SpecialKeyFilter& filter = ctx.server.special_keys();
  const Engine::LoadingReadScope during_load;  // CF6: DEBUG works while a full sync loads
  const Status st = ctx.server.engine().Snapshot(
      [&](ReadView& v, int64_t) { return MoveInto(CheckSpecialKeys(v, filter), &c); });
  if (!st.ok()) return ReplyError(ctx, st);
  ctx.reply.ArrayHeader(3);
  ctx.reply.SimpleString(filter.state_name());
  ctx.reply.Integer(static_cast<int64_t>(c.specials));
  ctx.reply.Integer(static_cast<int64_t>(c.missing));
}

// M7b D23: TidesKV only. One slot range of db 0; TTL mixes each key's exact deadline.
void DebugDigestSlots(ExecContext& ctx, const ArgList& argv) {
  if (argv.size() > 4 || (argv.size() == 4 && !EqualsIgnoreCase(argv[3], "ttl"))) {
    return ctx.reply.Error(kErrSyntax);
  }
  std::vector<cluster::SlotRange> ranges;
  if (Status st = MoveInto(cluster::ParseSlotRanges(argv[2]), &ranges); !st.ok()) return ReplyError(ctx, st);
  if (ranges.size() != 1) return ctx.reply.Error(kErrSyntax);
  Digest20 d{};
  const Engine::LoadingReadScope during_load;  // CF6
  const Status st = ctx.server.engine().Snapshot([&](ReadView& v, int64_t) {
    return MoveInto(SlotRangeDigest(v, ranges[0].start, ranges[0].end, argv.size() == 4), &d);
  });
  if (!st.ok()) return ReplyError(ctx, st);
  ctx.reply.SimpleString(DigestHex(d));
}

void Debug(ExecContext& ctx, const ArgList& argv) {
  const std::string sub = ToLower(argv[1]);
  if (sub == "help" && argv.size() == 2) return DebugHelp(ctx);
  if (sub == "digest" && argv.size() == 2) return DebugDigest(ctx);
  if (sub == "digest-value") return DebugDigestValue(ctx, argv);
  if (sub == "digest-index" && argv.size() == 2) return DebugDigestIndex(ctx);
  if (sub == "digest-slots" && argv.size() >= 3) return DebugDigestSlots(ctx, argv);
  if (sub == "special-keys" && argv.size() == 2) return DebugSpecialKeys(ctx);
  ctx.reply.Error(SubcommandSyntaxError(argv[1], "DEBUG"));
}

}  // namespace

void RegisterDebugCommands(CommandRegistry& r) {
  r.Register({"debug", -2, kCmdAdmin | kCmdNoScript | kCmdLoading | kCmdStale, 0, 0, 0, &Debug});
}

}  // namespace tideskv
