#include "commands/script_bridge.h"

#include <spdlog/spdlog.h>

#include <exception>

#include "commands/server_services.h"
#include "repl/replication.h"
#include "types/errors.h"

namespace tideskv {

ScriptBridge::ScriptBridge(ExecContext& caller) : caller_(caller) {
  session_.id = caller.session.id;
  session_.peer = caller.session.peer;
  session_.name = caller.session.name;
  session_.db = caller.session.db;
  session_.authenticated = true;  // the script runs as its caller
}

std::optional<std::string> ScriptBridge::Call(const std::vector<std::string>& argv, int resp,
                                              std::string* error) {
  const CommandSpec* spec = caller_.server.registry().Find(argv[0]);
  if (spec == nullptr) {
    *error = std::string(kScriptErrUnknownCommand);
    return std::nullopt;
  }
  if (!ArityOk(spec->arity, argv.size())) {
    *error = std::string(kScriptErrArity);
    return std::nullopt;
  }
  if ((spec->flags & kCmdNoScript) != 0) {
    *error = std::string(kScriptErrNoScript);
    return std::nullopt;
  }
  const bool is_write = (spec->flags & kCmdWrite) != 0;
  if (read_only_ && is_write) {  // the *_ro rule wins over the replica refusal
    *error = std::string(kScriptErrWriteInRo);
    return std::nullopt;
  }
  if (is_write && caller_.server.replication().IsReplica()) {
    *error = "-" + std::string(kErrReadOnlyReplica);
    return std::nullopt;
  }
  session_.protocol = resp;
  resp::ReplyBuilder reply(resp);
  ExecContext ctx{session_, reply, caller_.server, nullptr, caller_.ready, caller_.after_batch};
  try {
    spec->handler(ctx, argv);
  } catch (const std::exception& e) {
    spdlog::error("command from script failed: {}", e.what());
    *error = "-" + std::string(kErrInternal);
    return std::nullopt;
  }
  return reply.Take();
}

bool ScriptBridge::CommandExists(std::string_view name) const {
  return caller_.server.registry().Find(name) != nullptr;
}

}  // namespace tideskv
