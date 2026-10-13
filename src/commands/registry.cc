#include "commands/registry.h"

#include <stdexcept>
#include <utility>

#include "commands/multi_state.h"
#include "common/string_util.h"
#include "types/errors.h"

namespace tideskv {
namespace {

constexpr size_t kMaxEchoedArgs = 16;
constexpr size_t kMaxEchoedArgLen = 128;

std::string UnknownCommandMessage(const ArgList& argv) {
  std::string msg =
      "ERR unknown command '" + argv[0].substr(0, kMaxEchoedArgLen) + "', with args beginning with: ";
  for (size_t i = 1; i < argv.size() && i <= kMaxEchoedArgs; ++i) {
    msg += "'" + argv[i].substr(0, kMaxEchoedArgLen) + "' ";
  }
  return msg;
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
  if (!ctx.session.multi.dirty_exec) ctx.session.multi.queued.push_back(argv);  // multi.c:67
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
      {kCmdWrite, "write"},    {kCmdReadOnly, "readonly"}, {kCmdAdmin, "admin"},     {kCmdFast, "fast"},
      {kCmdNoAuth, "no-auth"}, {kCmdNoScript, "noscript"}, {kCmdNoMulti, "no_multi"}};
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
    return Reject(ctx, spec, "ERR wrong number of arguments for '" + spec->name + "' command");
  }
  if (!ctx.session.authenticated && (spec->flags & kCmdNoAuth) == 0) return Reject(ctx, spec, kErrNoAuth);
  if (ctx.session.multi.active && !RunsImmediately(spec->name)) return Queue(ctx, *spec, argv);
  spec->handler(ctx, argv);
}

}  // namespace tideskv
