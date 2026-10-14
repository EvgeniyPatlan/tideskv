#include <spdlog/spdlog.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "commands/all_commands.h"
#include "commands/registry.h"
#include "commands/reply_util.h"
#include "commands/script_bridge.h"
#include "commands/server_services.h"
#include "common/string_util.h"
#include "exec/engine.h"
#include "resp/reply_builder.h"
#include "script/script_engine.h"
#include "server/config.h"
#include "types/errors.h"

namespace tideskv {
namespace {

// H3: run inside a batch (our own, or EXEC's); a killed script is undone to its mark; the reply is
// sent only once our own batch committed.
void RunScript(ExecContext& ctx, const ScriptEngine::EvalRequest& req) {
  Engine& engine = ctx.server.engine();
  std::unique_ptr<Engine::Batch> own;
  if (!engine.InBatch()) {
    auto batch = engine.BeginBatch();
    if (!batch.ok()) return ReplyError(ctx, batch.status());
    own = std::move(batch).value();
  }
  auto mark = engine.BatchMark();
  if (!mark.ok()) return ReplyError(ctx, mark.status());
  resp::ReplyBuilder scratch(ctx.reply.protocol());
  ScriptBridge bridge(ctx);
  const ScriptEngine::EvalResult result = ctx.server.scripts().Eval(req, bridge, scratch);
  // Then drop the mark: a live one slows every later write of the batch (an EXEC may run many scripts).
  const Status undone = result.killed ? engine.BatchRollbackTo(*mark) : Status::Ok();
  if (!undone.ok()) {
    spdlog::error("rolling back a killed script: {}", undone.ToString());
  } else if (Status st = engine.BatchReleaseMark(*mark); !st.ok()) {
    spdlog::warn("releasing a script's batch mark: {}", st.ToString());
  }
  if (own) {
    const Status st = own->Commit();
    own.reset();  // release the gate before replying
    if (!st.ok()) return ReplyError(ctx, st);
  }
  ctx.reply.Raw(scratch.buffer());
}

void EvalGeneric(ExecContext& ctx, const ArgList& argv, bool by_sha, bool read_only) {
  if (by_sha && argv[1].size() != 40) return ctx.reply.Error(kErrNoScript);  // before numkeys (ev:559)
  int64_t numkeys = 0;
  if (!ParseInt64(argv[2], &numkeys)) return ctx.reply.Error(kErrNotInteger);
  if (numkeys > static_cast<int64_t>(argv.size()) - 3) return ctx.reply.Error(kErrNumkeysTooBig);
  if (numkeys < 0) return ctx.reply.Error(kErrNumkeysNegative);
  const auto split = argv.begin() + 3 + numkeys;
  const std::vector<std::string> keys(argv.begin() + 3, split);
  const std::vector<std::string> args(split, argv.end());
  RunScript(ctx, {argv[1], by_sha, read_only, &keys, &args,
                  ctx.server.config().Snapshot().busy_reply_threshold_ms});
}

void Eval(ExecContext& ctx, const ArgList& argv) { EvalGeneric(ctx, argv, false, false); }
void EvalSha(ExecContext& ctx, const ArgList& argv) { EvalGeneric(ctx, argv, true, false); }
void EvalRo(ExecContext& ctx, const ArgList& argv) { EvalGeneric(ctx, argv, false, true); }
void EvalShaRo(ExecContext& ctx, const ArgList& argv) { EvalGeneric(ctx, argv, true, true); }

struct ScriptSub {
  std::string_view name;
  int arity;
};
constexpr std::array<ScriptSub, 6> kScriptSubs = {
    {{"load", 3}, {"exists", -3}, {"flush", -2}, {"kill", 2}, {"debug", 3}, {"help", 2}}};

void ScriptHelp(ExecContext& ctx) {
  static constexpr std::array<std::string_view, 16> kLines = {
      "DEBUG (YES|SYNC|NO)",
      "    Set the debug mode for subsequent scripts executed.",
      "EXISTS <sha1> [<sha1> ...]",
      "    Return information about the existence of the scripts in the script cache.",
      "FLUSH [ASYNC|SYNC]",
      "    Flush the Lua scripts cache. Very dangerous on replicas.",
      "    When called without the optional mode argument, the behavior is determined by the",
      "    lazyfree-lazy-user-flush configuration directive. Valid modes are:",
      "    * ASYNC: Asynchronously flush the scripts cache.",
      "    * SYNC: Synchronously flush the scripts cache.",
      "KILL",
      "    Kill the currently executing Lua script.",
      "LOAD <script>",
      "    Load a script into the scripts cache without executing it.",
      "HELP",
      "    Print this help."};
  ctx.reply.ArrayHeader(kLines.size() + 1);
  ctx.reply.SimpleString("SCRIPT <subcommand> [<arg> [value] [opt] ...]. Subcommands are:");
  for (std::string_view line : kLines) ctx.reply.SimpleString(line);
}

void ScriptFlush(ExecContext& ctx, const ArgList& argv) {
  if (argv.size() > 3) return ctx.reply.Error(kErrScriptFlushOption);
  if (argv.size() == 3) {
    const std::string mode = ToLower(argv[2]);
    if (mode != "sync" && mode != "async") return ctx.reply.Error(kErrScriptFlushOption);
  }
  Engine& engine = ctx.server.engine();
  std::unique_ptr<Engine::Batch> own;  // holds the gate while the Lua state is replaced (H3)
  if (!engine.InBatch()) {
    auto batch = engine.BeginBatch();
    if (!batch.ok()) return ReplyError(ctx, batch.status());
    own = std::move(batch).value();
  }
  ctx.server.scripts().Flush();
  own.reset();  // nothing was written: the batch rolls back
  ctx.reply.SimpleString("OK");
}

void ScriptDebug(ExecContext& ctx, const ArgList& argv) {
  const std::string mode = ToLower(argv[2]);
  if (mode == "no") return ctx.reply.SimpleString("OK");
  if (mode == "yes" || mode == "sync") return ctx.reply.Error(kErrScriptDebugUnsupported);
  ctx.reply.Error(kErrScriptDebugOption);
}

// LOAD and EXISTS take ScriptEngine's mutex, which a running script holds. Taking the gate first
// (lock order gate -> mutex) means a call that arrives before the script times out waits on the
// gate, where the timeout wakes it with BUSY, never on the mutex (H6). Inside EXEC the Read runs in
// the batch, and the mutex is free.
void ScriptExists(ExecContext& ctx, const ArgList& argv) {
  const std::vector<std::string> shas(argv.begin() + 2, argv.end());
  std::vector<bool> found;
  const Status st = ctx.server.engine().Read([&](ReadView&, int64_t) {
    found = ctx.server.scripts().Exists(shas);
    return Status::Ok();
  });
  if (!st.ok()) return ReplyError(ctx, st);
  ctx.reply.ArrayHeader(found.size());
  for (bool f : found) ctx.reply.Integer(f ? 1 : 0);
}

void ScriptLoad(ExecContext& ctx, const ArgList& argv) {
  Result<std::string> sha = Status::Internal("script not loaded");
  const Status st = ctx.server.engine().Read([&](ReadView&, int64_t) {
    sha = ctx.server.scripts().Load(argv[2]);
    return Status::Ok();
  });
  if (!st.ok()) return ReplyError(ctx, st);
  if (!sha.ok()) return ReplyError(ctx, sha.status());
  ctx.reply.Bulk(*sha);
}

void Script(ExecContext& ctx, const ArgList& argv) {
  if (std::string err = ScriptSubcommandError(argv); !err.empty()) return ctx.reply.Error(err);
  const std::string sub = ToLower(argv[1]);
  // A timed-out script holds the gate and the engine's mutex: answer at once (Task 9).
  if (sub != "kill" && ctx.server.scripts().TimedOut()) return ctx.reply.Error(kErrBusyScript);
  if (sub == "load") return ScriptLoad(ctx, argv);
  if (sub == "exists") return ScriptExists(ctx, argv);
  if (sub == "flush") return ScriptFlush(ctx, argv);
  if (sub == "debug") return ScriptDebug(ctx, argv);
  if (sub == "help") return ScriptHelp(ctx);
  if (ctx.server.scripts().Kill()) return ctx.reply.SimpleString("OK");
  ctx.reply.Error(kErrNotBusy);
}

}  // namespace

std::string ScriptSubcommandError(const ArgList& argv) {
  const std::string sub = ToLower(argv[1]);
  for (const ScriptSub& s : kScriptSubs) {
    if (s.name != sub) continue;
    if (ArityOk(s.arity, argv.size())) return {};
    return ArityError("script|" + sub);
  }
  return UnknownSubcommandError(argv[1], "SCRIPT");
}

void RegisterScriptCommands(CommandRegistry& r) {
  r.Register({"eval", -3, kCmdNoScript, 0, 0, 0, &Eval});
  r.Register({"evalsha", -3, kCmdNoScript, 0, 0, 0, &EvalSha});
  r.Register({"eval_ro", -3, kCmdNoScript | kCmdReadOnly, 0, 0, 0, &EvalRo});
  r.Register({"evalsha_ro", -3, kCmdNoScript | kCmdReadOnly, 0, 0, 0, &EvalShaRo});
  r.Register({"script", -2, kCmdNoScript | kCmdAllowBusy, 0, 0, 0, &Script});
}

}  // namespace tideskv
