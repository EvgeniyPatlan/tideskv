#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "commands/all_commands.h"
#include "commands/cmd_util.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "common/string_util.h"
#include "exec/engine.h"
#include "types/digest.h"

// DEBUG: only the DIGEST / DIGEST-VALUE subcommands (debug.c), with Redis's exact algorithm.
namespace tideskv {
namespace {

void DebugHelp(ExecContext& ctx) {
  static constexpr std::array<std::string_view, 6> kLines = {
      "DIGEST",
      "    Output a hex signature representing the current DB content.",
      "DIGEST-VALUE <key> [<key> ...]",
      "    Output a hex signature of the values of all the specified keys.",
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

void Debug(ExecContext& ctx, const ArgList& argv) {
  const std::string sub = ToLower(argv[1]);
  if (sub == "help" && argv.size() == 2) return DebugHelp(ctx);
  if (sub == "digest" && argv.size() == 2) return DebugDigest(ctx);
  if (sub == "digest-value") return DebugDigestValue(ctx, argv);
  ctx.reply.Error(SubcommandSyntaxError(argv[1], "DEBUG"));
}

}  // namespace

void RegisterDebugCommands(CommandRegistry& r) {
  r.Register({"debug", -2, kCmdAdmin | kCmdNoScript | kCmdLoading | kCmdStale, 0, 0, 0, &Debug});
}

}  // namespace tideskv
