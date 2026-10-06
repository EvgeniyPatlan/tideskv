#include "commands/registry.h"

#include <stdexcept>
#include <utility>

#include "common/string_util.h"

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
      {kCmdWrite, "write"}, {kCmdReadOnly, "readonly"}, {kCmdAdmin, "admin"},
      {kCmdFast, "fast"},   {kCmdNoAuth, "no-auth"},    {kCmdNoScript, "noscript"}};
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
  if (spec == nullptr) {
    ctx.reply.Error(UnknownCommandMessage(argv));
    return;
  }
  if (!ArityOk(spec->arity, argv.size())) {
    ctx.reply.Error("ERR wrong number of arguments for '" + spec->name + "' command");
    return;
  }
  if (!ctx.session.authenticated && (spec->flags & kCmdNoAuth) == 0) {
    ctx.reply.Error("NOAUTH Authentication required.");
    return;
  }
  spec->handler(ctx, argv);
}

}  // namespace tideskv
