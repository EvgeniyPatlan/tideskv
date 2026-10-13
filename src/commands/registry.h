#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "encoding/meta_codec.h"
#include "resp/reply_builder.h"
#include "server/session.h"

namespace tideskv {

class ServerServices;

// What a blocking command asks for when every key is empty (M4 plan F6).
struct BlockRequest {
  encoding::ValueType type = encoding::ValueType::kList;  // kList or kZSet
  std::vector<std::string> keys;                          // in argument order (duplicates allowed)
  int64_t timeout_ms = 0;                                 // relative; 0 = forever
};

class BlockHandler {
 public:
  virtual ~BlockHandler() = default;
  // Parks the command. The handler writes no reply; it comes later (served, timeout, CLIENT UNBLOCK).
  virtual void Block(const BlockRequest& request) = 0;
};

// A key a write may have made non-empty for blocked clients.
struct ReadyKey {
  uint8_t db = 0;
  std::string key;
};
using ReadyKeys = std::vector<ReadyKey>;

struct ExecContext {
  Session& session;
  resp::ReplyBuilder& reply;
  ServerServices& server;
  BlockHandler* block = nullptr;  // null: this context cannot block (answer as timed out; MULTI, Lua)
  ReadyKeys* ready = nullptr;     // where SignalReady records keys; null: nobody serves waiters
  // Inside EXEC (M5a G4): actions to run once the batch has committed and released the gate.
  std::vector<std::function<void()>>* after_batch = nullptr;
};

enum CommandFlag : uint32_t {
  kCmdWrite = 1u << 0,
  kCmdReadOnly = 1u << 1,
  kCmdAdmin = 1u << 2,
  kCmdFast = 1u << 3,
  kCmdNoAuth = 1u << 4,  // allowed before AUTH
  kCmdNoScript = 1u << 5,
  kCmdNoMulti = 1u << 6,    // refused inside MULTI (Redis CMD_NO_MULTI)
  kCmdAllowBusy = 1u << 7,  // runs while a script is busy (Redis CMD_ALLOW_BUSY)
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

// Validates (unknown → arity → auth → busy script, Redis 7 order), queues inside MULTI (M5a G3), and runs one
// command.
void Dispatch(const CommandRegistry& registry, ExecContext& ctx, const ArgList& argv);

}  // namespace tideskv
