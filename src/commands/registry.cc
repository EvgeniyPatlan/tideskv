#include "commands/registry.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "commands/all_commands.h"
#include "commands/multi_state.h"
#include "commands/reply_util.h"
#include "commands/server_services.h"
#include "common/string_util.h"
#include "repl/replication.h"
#include "script/script_engine.h"
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

void Queue(ExecContext& ctx, const CommandSpec& spec, const ArgList& argv) {
  if ((spec.flags & kCmdNoMulti) != 0) return Reject(ctx, &spec, kErrNotAllowedInMulti);
  if (spec.name == "script") {  // subcommand lookup precedes queueing in Redis processCommand
    if (std::string err = ScriptSubcommandError(argv); !err.empty()) return Reject(ctx, &spec, err);
  }
  const auto& w = ctx.session.watch;
  const bool doomed = ctx.session.multi.dirty_exec || (w != nullptr && w->dirty());
  if (!doomed) {  // multi.c:67
    ctx.session.multi.queued.push_back(argv);
    ctx.session.multi.has_write |= (spec.flags & kCmdWrite) != 0;
  }
  ctx.reply.SimpleString("QUEUED");
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
      {kCmdWrite, "write"},      {kCmdReadOnly, "readonly"},   {kCmdAdmin, "admin"},
      {kCmdFast, "fast"},        {kCmdNoAuth, "no-auth"},      {kCmdNoScript, "noscript"},
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

void Dispatch(const CommandRegistry& registry, ExecContext& ctx, const ArgList& argv) {
  if (argv.empty()) return;
  const CommandSpec* spec = registry.Find(argv[0]);
  if (spec == nullptr) return Reject(ctx, nullptr, UnknownCommandMessage(argv));
  if (!ArityOk(spec->arity, argv.size())) {
    return Reject(ctx, spec, ArityError(spec->name));
  }
  if (!ctx.session.authenticated && (spec->flags & kCmdNoAuth) == 0) return Reject(ctx, spec, kErrNoAuth);
  if ((spec->flags & kCmdWrite) != 0 && ctx.server.replication().IsReplica()) {
    return Reject(ctx, spec, kErrReadOnlyReplica);  // sv:4060, before BUSY; MULTI -> dirty
  }
  if ((spec->flags & kCmdAllowBusy) == 0 && ctx.server.scripts().TimedOut()) {
    return Reject(ctx, spec, kErrBusyScript);  // flags MULTI; EXEC becomes EXECABORT (sv:4115)
  }
  if (ctx.session.multi.active && !RunsImmediately(spec->name)) return Queue(ctx, *spec, argv);
  spec->handler(ctx, argv);
}

}  // namespace tideskv
