#pragma once
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "resp/reply_builder.h"
#include "script/redis_api.h"

struct lua_State;

namespace tideskv {

// The Redis 7.2 Lua environment (eval.c scriptingInit, script_lua.c luaCallFunction): one
// lua_State, sandboxed and protected as Redis does it. Not thread-safe: callers serialise.
class LuaSandbox {
 public:
  static std::unique_ptr<LuaSandbox> Create();  // aborts if Lua cannot start
  ~LuaSandbox();                                // full GC, then lua_close (CVE-2024-46981)
  LuaSandbox(const LuaSandbox&) = delete;
  LuaSandbox& operator=(const LuaSandbox&) = delete;

  // Compiles `code` as registry["f_<sha>"]. UserError "ERR Error compiling script ..." on errors.
  Status Compile(const std::string& sha, std::string_view code);

  struct RunOptions {
    bool hook = true;  // the count hook that asks host.ShouldKill()
  };
  struct RunResult {
    bool killed = false;
  };
  // Runs the compiled f_<sha> in EVAL mode with fresh KEYS/ARGV and writes its reply to `out`.
  RunResult Run(const std::string& sha, const std::vector<std::string>& keys,
                const std::vector<std::string>& args, SandboxHost& host, const RunOptions& opts,
                resp::ReplyBuilder& out);

  size_t memory_bytes() const;  // lua_gc(LUA_GCCOUNT) * 1024 (used_memory_lua)

 private:
  explicit LuaSandbox(lua_State* L) : L_(L) {}

  lua_State* L_;
  int gc_runs_ = 0;
};

}  // namespace tideskv
