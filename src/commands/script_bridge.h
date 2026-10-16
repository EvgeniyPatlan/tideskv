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
  void SetCrossSlotAllowed(bool allowed) override { allow_cross_slot_ = allowed; }
  // The EVAL's declared keys (already routed at dispatch): their slot is the script's first slot (D15).
  void SetDeclaredKeys(const std::vector<std::string>& keys);
  std::optional<std::string> Call(const std::vector<std::string>& argv, int resp,
                                  std::string* error) override;
  bool CommandExists(std::string_view name) const override;

 private:
  ExecContext& caller_;
  Session session_;
  bool read_only_ = false;
  bool wrote_ = false;  // a write command already ran (SCRIPT_WRITE_DIRTY)
  bool allow_cross_slot_ = true;
  int first_slot_ = -1;  // Redis original_c->slot
};

}  // namespace tideskv
