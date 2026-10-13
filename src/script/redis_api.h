#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;
struct lua_Debug;

namespace tideskv {

// What a running script asks of the server (M5b H5).
class SandboxHost {
 public:
  virtual ~SandboxHost() = default;
  // redis.call/pcall: the reply bytes in RESP `resp`, or nullopt with *error = "msg" or "-CODE msg".
  virtual std::optional<std::string> Call(const std::vector<std::string>& argv, int resp,
                                          std::string* error) noexcept = 0;
  virtual bool CommandExists(std::string_view name) const noexcept = 0;  // redis.acl_check_cmd
  virtual void Log(int level, std::string_view msg) noexcept = 0;        // redis.log, level 0..3
  // Polled by the count hook every 100000 instructions until it first returns true; the kill is
  // then sticky for the rest of the run (raised again on every line, so pcall cannot swallow it).
  virtual bool ShouldKill() noexcept = 0;
};

// The state of the script being run (Redis's scriptRunCtx, as far as the Lua API needs it).
struct ScriptRun {
  SandboxHost* host = nullptr;
  int resp = 2;  // redis.setresp; 2 at the start of every run (script.c:218)
  bool killed = false;
};

// Builds the `redis` table, the global `pcall` and math.random/randomseed (script_lua.c
// luaRegisterRedisAPI + eval.c scriptingInit). Call inside lua_cpcall at state creation.
void RegisterRedisApi(lua_State* L);

ScriptRun* CurrentRun(lua_State* L);  // nullptr when no script runs
void SetCurrentRun(lua_State* L, ScriptRun* run);

}  // namespace tideskv

// luaMaskCountHook: raises the SCRIPT KILL error when the host says so, then keeps raising on
// every line so pcall cannot swallow it.
extern "C" void ScriptCountHook(lua_State* L, lua_Debug* ar) noexcept;
