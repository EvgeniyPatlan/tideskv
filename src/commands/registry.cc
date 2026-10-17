#include "commands/registry.h"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "commands/all_commands.h"
#include "commands/cluster_routing.h"
#include "commands/multi_state.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "common/string_util.h"
#include "exec/engine.h"
#include "exec/thread_commit_seq.h"
#include "repl/replication.h"
#include "script/script_engine.h"
#include "server/client_pause.h"
#include "server/watch.h"
#include "types/errors.h"

namespace tideskv {
namespace {

constexpr size_t kMaxEchoedLen = 128;

// C "%.<max>s": at most `max` bytes, ended early by an embedded NUL.
std::string_view CStrPrefix(const std::string& s, size_t max) {
  std::string_view v(s.data(), std::min(s.size(), max));
  return v.substr(0, v.find('\0'));
}

// Redis 7.2 processCommand: the name is "%.128s"; args are "'%.*s' " while the args text is
// shorter than 128 bytes, each limited to the room left.
std::string UnknownCommandMessage(const ArgList& argv) {
  std::string args;
  for (size_t i = 1; i < argv.size() && args.size() < kMaxEchoedLen; ++i) {
    const size_t room = kMaxEchoedLen - args.size();
    args.append("'").append(CStrPrefix(argv[i], room)).append("' ");
  }
  return "ERR unknown command '" + std::string(CStrPrefix(argv[0], kMaxEchoedLen)) +
         "', with args beginning with: " + args;
}

// Run at once even inside MULTI (server.c:4155-4161).
bool RunsImmediately(std::string_view name) {
  return name == "exec" || name == "discard" || name == "multi" || name == "watch" || name == "quit" ||
         name == "reset";
}

// A command refused before it runs: replies `error` and, inside MULTI, dooms the transaction (Redis
// flagTransaction). A refused EXEC discards the transaction instead and names the reason, without
// "ERR " (execCommandAbort, multi.c:136).
void Reject(ExecContext& ctx, const CommandSpec* spec, std::string_view error) {
  if (spec != nullptr && spec->name == "exec") {
    DiscardTransaction(ctx);
    if (error.starts_with("ERR ")) error.remove_prefix(4);
    return ctx.reply.Error(std::string(kErrExecAbortBecause).append(error));
  }
  if (ctx.session.multi.active) ctx.session.multi.dirty_exec = true;
  ctx.reply.Error(error);
}

// A routing refusal (D14): inside MULTI it dooms the transaction like any refusal; a refused EXEC discards it
// and replies the bare -MOVED/-CROSSSLOT/-CLUSTERDOWN (server.c:3959-3966), not EXECABORT.
void RejectRouted(ExecContext& ctx, const CommandSpec& spec, std::string_view error) {
  if (spec.name != "exec") return Reject(ctx, &spec, error);
  DiscardTransaction(ctx);
  ctx.reply.Error(error);
}

bool IsEvalFamily(std::string_view n) {
  return n == "eval" || n == "evalsha" || n == "eval_ro" || n == "evalsha_ro";
}

// EVAL and EVALSHA carry Redis's may-replicate flag whatever the script does (server.c:3914).
bool IsMayReplicateScript(std::string_view n) { return n == "eval" || n == "evalsha"; }

uint32_t ScriptSubcommandFlags(uint32_t base, const ArgList& argv) {
  if (argv.size() < 2) return base;
  const std::string sub = ToLower(argv[1]);
  if (sub == "help") return base | kCmdLoading | kCmdStale;
  if (sub == "load") return base | kCmdStale;
  return base;
}

// M7b D14: CLUSTER IMPORT / IMPORTDATA write, so NOREPLICAS, READONLY and CLIENT PAUSE apply, and are refused
// while loading or stale; the other subcommands keep CLUSTER's loading|stale (the controller's probes).
uint32_t ClusterSubcommandFlags(uint32_t base, const ArgList& argv) {
  if (argv.size() < 2) return base;
  const std::string sub = ToLower(argv[1]);
  if (sub == "import" || sub == "importdata")
    return (base & ~(kCmdLoading | kCmdStale)) | kCmdWrite | kCmdAdmin;
  return base;
}

void Queue(ExecContext& ctx, const CommandSpec& spec, const ArgList& argv, uint32_t flags) {
  if ((spec.flags & kCmdNoMulti) != 0) return Reject(ctx, &spec, kErrNotAllowedInMulti);
  if (spec.name == "script") {  // subcommand lookup precedes queueing in Redis processCommand
    if (std::string err = ScriptSubcommandError(argv); !err.empty()) return Reject(ctx, &spec, err);
  }
  const auto& w = ctx.session.watch;
  const bool doomed = ctx.session.multi.dirty_exec || (w != nullptr && w->dirty());
  if (!doomed) {  // multi.c:67
    ctx.session.multi.queued.push_back(argv);
    ctx.session.multi.has_write |= (flags & kCmdWrite) != 0;
    ctx.session.multi.has_non_stale |= (flags & kCmdStale) == 0;
    ctx.session.multi.has_non_loading |= (flags & kCmdLoading) == 0;
    ctx.session.multi.has_may_replicate |= (flags & kCmdWrite) != 0 || IsMayReplicateScript(spec.name);
  }
  ctx.reply.SimpleString("QUEUED");
}

// Redis is_may_replicate_command (server.c:3914): CLIENT PAUSE WRITE postpones these.
bool MayReplicate(const Session& s, const CommandSpec& spec, const CallFlags& call) {
  return call.is_write || IsMayReplicateScript(spec.name) ||
         (spec.name == "exec" && s.multi.has_may_replicate);
}

// blockPostponeClient (server.c:4146, M7a D21): true = postponed, no reply now; the connection re-dispatches
// argv when the pause resumes it. Otherwise the call runs now; a may-replicate one stays counted in flight
// (Session::write_in_flight) until DispatchAndServe ends it, so CLIENT PAUSE waits for it (review fix 1).
bool Postpone(ExecContext& ctx, const CommandSpec& spec, const CallFlags& call, const ArgList& argv) {
  switch (ctx.server.pause().Admit(MayReplicate(ctx.session, spec, call), ctx.session.link)) {
    case ClientPause::Admission::kParked:
      ctx.session.postponed = argv;  // Resume runs on this connection's thread, after Dispatch returned
      return true;
    case ClientPause::Admission::kRunCounted:
      ctx.session.write_in_flight = true;
      return false;
    case ClientPause::Admission::kRun:
      return false;
  }
  return false;
}

}  // namespace

void CommandRegistry::Register(CommandSpec spec) {
  spec.name = ToLower(spec.name);
  std::string key = spec.name;
  if (!commands_.emplace(std::move(key), std::move(spec)).second) {
    throw std::logic_error("duplicate command registration");
  }
}

const CommandSpec* CommandRegistry::Find(std::string_view name) const {
  const auto it = commands_.find(ToLower(name));
  return it == commands_.end() ? nullptr : &it->second;
}

std::vector<std::string_view> FlagNames(uint32_t flags) {
  static constexpr std::pair<CommandFlag, std::string_view> kNames[] = {
      {kCmdWrite, "write"},      {kCmdReadOnly, "readonly"},   {kCmdAdmin, "admin"},     {kCmdFast, "fast"},
      {kCmdNoAuth, "no-auth"},   {kCmdNoScript, "noscript"},   {kCmdLoading, "loading"}, {kCmdStale, "stale"},
      {kCmdNoMulti, "no_multi"}, {kCmdAllowBusy, "allow_busy"}};
  std::vector<std::string_view> out;
  for (const auto& [flag, name] : kNames) {
    if ((flags & flag) != 0) out.push_back(name);
  }
  return out;
}

bool ArityOk(int arity, size_t argc) {
  const auto n = static_cast<int64_t>(argc);
  return arity >= 0 ? n == arity : n >= -arity;
}

uint32_t EffectiveFlags(ExecContext& ctx, const CommandSpec& spec, const ArgList& argv) {
  if (spec.name == "script") return ScriptSubcommandFlags(spec.flags, argv);
  if (spec.name == "cluster") return ClusterSubcommandFlags(spec.flags, argv);
  if (!IsEvalFamily(spec.name) || argv.size() < 2) return spec.flags;
  const std::optional<ScriptFlags> f =
      ctx.server.scripts().PeekFlags(argv[1], spec.name.starts_with("evalsha"));
  if (!f) return spec.flags;                             // unknown sha / bad shebang / busy: as registered
  if (!f->shebang) return spec.flags & ~kCmdStale;       // compat mode: MASTERDOWN on 7.2.5 (live)
  uint32_t out = spec.flags & ~(kCmdStale | kCmdWrite);  // scriptFlagsToCmdFlags (script.c:111)
  if (!f->no_writes) out |= kCmdWrite;
  if (f->allow_stale) out |= kCmdStale;
  return out;
}

CallFlags DeriveCallFlags(const Session& session, const CommandSpec& spec, uint32_t flags) {
  const bool exec = spec.name == "exec";
  const MultiState& m = session.multi;
  return {(flags & kCmdWrite) != 0 || (exec && m.has_write),
          (flags & kCmdStale) == 0 || (exec && m.has_non_stale),
          (flags & kCmdLoading) == 0 || (exec && m.has_non_loading)};
}

// The first state-based refusal in Redis order (server.c:4057 NOREPLICAS, server.c:4060 READONLY, 4097
// MASTERDOWN, 4104 LOADING), or empty. MULTI -> dirty is Reject's job; BUSY after.
std::string_view StateRefusal(ExecContext& ctx, const CallFlags& call) {
  const Replication& repl = ctx.server.replication();
  if (call.is_write && !repl.GoodReplicasOk()) return kErrNoReplicas;  // server.c:4057, before READONLY
  if (call.is_write && repl.IsReplica()) return kErrReadOnlyReplica;
  if (call.deny_stale && repl.StaleRefusing()) return kErrMasterDown;
  if (call.deny_loading && ctx.server.engine().loading()) return kErrLoading;  // server.c:4104, before BUSY
  return {};
}

namespace {

void DispatchFound(ExecContext& ctx, const CommandSpec* spec, const ArgList& argv) {
  if (spec == nullptr) return Reject(ctx, nullptr, UnknownCommandMessage(argv));
  if (!ArityOk(spec->arity, argv.size())) {
    return Reject(ctx, spec, ArityError(spec->name));
  }
  if (!ctx.session.authenticated && (spec->flags & kCmdNoAuth) == 0) return Reject(ctx, spec, kErrNoAuth);
  const uint32_t flags = EffectiveFlags(ctx, *spec, argv);
  const CallFlags call = DeriveCallFlags(ctx.session, *spec, flags);
  // D13: Redis getNodeByQuery, after auth and the effective flags, before NOREPLICAS/READONLY/.../BUSY.
  if (std::string e = ClusterRefusal(ctx, *spec, argv, call.is_write); !e.empty()) {
    return RejectRouted(ctx, *spec, e);
  }
  if (std::string_view e = StateRefusal(ctx, call); !e.empty()) return Reject(ctx, spec, e);
  if ((spec->flags & kCmdAllowBusy) == 0 && ctx.server.scripts().TimedOut()) {
    return Reject(ctx, spec, kErrBusyScript);  // flags MULTI; EXEC becomes EXECABORT (sv:4115)
  }
  if (Postpone(ctx, *spec, call, argv)) return;  // CLIENT PAUSE (D21), before queueing (server.c:4146)
  if (ctx.session.multi.active && !RunsImmediately(spec->name)) return Queue(ctx, *spec, argv, flags);
  const uint64_t commits = ThreadCommitCount();
  spec->handler(ctx, argv);
  if (ThreadCommitCount() != commits) ctx.session.woff = ThreadLastCommitSeq();  // WAIT (M6b D9, M7a D24)
}

}  // namespace

void Dispatch(const CommandRegistry& registry, ExecContext& ctx, const ArgList& argv) {
  if (argv.empty()) return;
  const CommandSpec* spec = registry.Find(argv[0]);
  DispatchFound(ctx, spec, argv);
  // Redis resetClient (networking.c:2100): ASKING covers exactly the next command (refused or not), or a
  // whole MULTI..EXEC; a postponed command has not run yet.
  const bool keep = (spec != nullptr && spec->name == "asking") || ctx.session.multi.active ||
                    ctx.session.postponed.has_value();
  if (!keep) ctx.session.asking = false;
}

}  // namespace tideskv
