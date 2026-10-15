#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/status.h"
#include "resp/reply_builder.h"
#include "script/lua_sandbox.h"
#include "script/shebang.h"

namespace tideskv {

// How a script reaches commands (implemented by ScriptBridge in the commands layer).
class ScriptCaller {
 public:
  virtual ~ScriptCaller() = default;
  virtual void SetReadOnly(bool read_only) = 0;  // EVAL_RO / no-writes (H4)
  virtual std::optional<std::string> Call(const std::vector<std::string>& argv, int resp,
                                          std::string* error) = 0;
  virtual bool CommandExists(std::string_view name) const = 0;
};

// The script cache, compile/lookup, the *_RO rule, and the busy/timeout/kill state (M5b H2-H7).
class ScriptEngine {
 public:
  ScriptEngine();

  struct EvalRequest {
    std::string_view script;  // the body (EVAL) or the 40-character sha (EVALSHA)
    bool by_sha = false;
    bool read_only_command = false;  // EVAL_RO / EVALSHA_RO
    const std::vector<std::string>* keys = nullptr;
    const std::vector<std::string>* args = nullptr;
    int64_t busy_threshold_ms = 5000;  // 0 = no hook
    bool replica = false;  // this node is a replica: a shebang script without no-writes is refused
  };
  struct EvalResult {
    bool killed = false;
  };
  // Under mu_: looks up or compiles the script, applies the *_RO rule (H8), runs it and writes the
  // reply or error to `out`. The caller holds the gate (a batch, H3).
  EvalResult Eval(const EvalRequest& req, ScriptCaller& caller, resp::ReplyBuilder& out);
  Result<std::string> Load(std::string_view body);                       // SCRIPT LOAD: the sha
  std::vector<bool> Exists(const std::vector<std::string>& shas) const;  // case-insensitive
  void Flush();  // SCRIPT FLUSH: empty cache, fresh Lua state; the caller holds the gate
  // Flags of a cached script, else (EVAL bodies only) of its parsed shebang; nullopt for an unknown sha, a
  // bad shebang, or while a script runs (mu_ busy: the BUSY check answers).
  std::optional<ScriptFlags> PeekFlags(std::string_view script, bool by_sha) const;
  // Redis evalGetCommandFlags: true if the script has a shebang without no-writes (it counts as a write).
  bool DeclaresWrites(std::string_view script, bool by_sha) const;
  bool TimedOut() const noexcept;   // a script runs past busy_threshold_ms (BUSY)
  bool Kill() noexcept;             // SCRIPT KILL: false = NOTBUSY
  void KillForShutdown() noexcept;  // SHUTDOWN NOSAVE / App::Stop: any running or later script dies
  void SetOnTimeout(std::function<void()> fn);  // called once per timed-out run (Engine::WakeWaiters)
  size_t cached_scripts() const noexcept { return cached_.load(); }  // INFO number_of_cached_scripts
  size_t lua_memory() const noexcept { return lua_memory_.load(); }  // INFO used_memory_lua
  void ForceTimedOutForTests(bool v) noexcept { forced_timed_out_ = v; }

 private:
  struct Script {
    ScriptFlags flags;
  };
  class Host;  // SandboxHost adapter

  // Returns the sha, compiling and caching on first sight; the error status on failure.
  Result<std::string> LookupOrCompile(std::string_view script, bool by_sha);

  mutable std::mutex mu_;  // the Lua state and the cache (lock order: after the Engine gate)
  std::unique_ptr<LuaSandbox> sandbox_;
  std::unordered_map<std::string, Script> scripts_;  // key: lowercase sha
  // One state word, so Kill() can only ever hit the run that timed out (never a later one).
  enum Phase : int { kIdle, kRunning, kTimedOut, kKilled };
  std::atomic<int> phase_{kIdle};
  std::atomic<bool> shutdown_{false};
  std::atomic<bool> forced_timed_out_{false};
  std::atomic<size_t> cached_{0};
  std::atomic<size_t> lua_memory_{0};
  std::function<void()> on_timeout_;
};

}  // namespace tideskv
