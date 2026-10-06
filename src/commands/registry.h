#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "resp/reply_builder.h"
#include "server/session.h"

namespace tideskv {

class ServerServices;

struct ExecContext {
  Session& session;
  resp::ReplyBuilder& reply;
  ServerServices& server;
};

enum CommandFlag : uint32_t {
  kCmdWrite = 1u << 0,
  kCmdReadOnly = 1u << 1,
  kCmdAdmin = 1u << 2,
  kCmdFast = 1u << 3,
  kCmdNoAuth = 1u << 4,  // allowed before AUTH
  kCmdNoScript = 1u << 5,
};

using ArgList = std::vector<std::string>;
using CommandHandler = void (*)(ExecContext& ctx, const ArgList& argv);

struct CommandSpec {
  std::string name;  // lowercase
  int arity;         // N: exactly N args; -N: at least N (argv[0] counts)
  uint32_t flags;
  int first_key;  // Redis key-spec triple; 0,0,0 = no keys
  int last_key;
  int key_step;
  CommandHandler handler;
};

class CommandRegistry {
 public:
  void Register(CommandSpec spec);  // throws std::logic_error on duplicates
  const CommandSpec* Find(std::string_view name) const;
  size_t size() const { return commands_.size(); }
  const std::map<std::string, CommandSpec, std::less<>>& all() const { return commands_; }

 private:
  std::map<std::string, CommandSpec, std::less<>> commands_;
};

std::vector<std::string_view> FlagNames(uint32_t flags);
bool ArityOk(int arity, size_t argc);

// Validates (unknown → arity → auth, Redis 7 order) and runs one command.
void Dispatch(const CommandRegistry& registry, ExecContext& ctx, const ArgList& argv);

}  // namespace tideskv
