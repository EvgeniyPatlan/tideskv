#pragma once
#include "commands/registry.h"
#include "script/script_engine.h"

namespace tideskv {

// redis.call/pcall into command handlers (M5b H4): the script's own session (the caller's db,
// RESP from redis.setresp), no blocking, ready keys go to the caller's list.
class ScriptBridge final : public ScriptCaller {
 public:
  explicit ScriptBridge(ExecContext& caller);
  void SetReadOnly(bool read_only) override { read_only_ = read_only; }
  std::optional<std::string> Call(const std::vector<std::string>& argv, int resp,
                                  std::string* error) override;
  bool CommandExists(std::string_view name) const override;

 private:
  ExecContext& caller_;
  Session session_;
  bool read_only_ = false;
};

}  // namespace tideskv
