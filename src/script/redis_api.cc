#include "script/redis_api.h"

#include <spdlog/spdlog.h>

#include <array>
#include <climits>
#include <cmath>

#include "common/string_util.h"
#include "common/version.h"
#include "resp/reply_parser.h"
#include "script/lua_api.h"
#include "script/lua_convert.h"
#include "script/rand48.h"
#include "script/sha1hex.h"
#include "types/errors.h"

// Port of Redis 7.2.16 script_lua.c (sl) and eval.c (ev). Every Lua-facing function that owns a
// C++ object is a trampoline (lua-integration.md §6): XxxImpl does all the C++ work and returns
// the number of results, or kRaise with the error value already pushed; the noexcept trampoline
// calls lua_error only after every C++ object of XxxImpl is gone.

namespace tideskv {
namespace {

constexpr int kRaise = -1;

const char kRunKey = 0;  // its address is the registry key of the current ScriptRun

constexpr int RedisVersionNum(std::string_view v) {  // "7.2.0" -> 0x00070200 (version.h)
  std::array<int, 3> parts{0, 0, 0};
  size_t i = 0;
  for (const char c : v) {
    if (c == '.') {
      ++i;
    } else {
      parts.at(i) = parts.at(i) * 10 + (c - '0');
    }
  }
  return (parts[0] << 16) | (parts[1] << 8) | parts[2];
}
static_assert(RedisVersionNum("7.2.0") == 0x00070200);

// (int)lua_tonumber(L, idx) as Redis writes it, without UB: x86-64 gives INT_MIN out of range.
int ToInt(lua_State* L, int idx) {
  const double d = lua_tonumber(L, idx);
  if (!(d > static_cast<double>(INT_MIN) - 1 && d < static_cast<double>(INT_MAX) + 1)) return INT_MIN;
  return static_cast<int>(d);
}

// luaPushError + "return luaError(lua)": the error table is left for the trampoline to raise.
int PushLibError(lua_State* L, std::string_view msg) {
  PushErrorTable(L, msg);
  return kRaise;
}

// A failed invariant (no ScriptRun, unparsable reply, exception): "-ERR internal error".
int PushInternalError(lua_State* L) {
  std::string err("-");
  err += kErrInternal;
  PushErrorTable(L, err);
  return kRaise;
}

// Runs impl, turning an escaped exception into an internal error. No C++ object outlives it.
int Guarded(lua_State* L, int (*impl)(lua_State*)) noexcept {
  bool failed = false;
  int n = 0;
  try {
    n = impl(L);
  } catch (...) {
    failed = true;
  }
  if (!failed) return n;
  lua_settop(L, 0);
  return PushInternalError(L);
}

// The second half of every trampoline. Has no C++ objects, so lua_error may longjmp out of it.
int RaiseIfAsked(lua_State* L, int n) { return n >= 0 ? n : lua_error(L); }

// luaRedisGenericCommand (sl:893-1006).
int GenericCommandImpl(lua_State* L, bool raise) {
  std::string error;
  std::optional<std::vector<std::string>> argv = ArgsToArgv(L, &error);
  lua_settop(L, 0);  // luaArgsToRedisArgv pops the arguments (sl:846-850)
  if (!argv) {
    PushErrorTable(L, error);
    return raise ? kRaise : 1;
  }
  ScriptRun* run = CurrentRun(L);
  if (run == nullptr) return PushInternalError(L);
  std::optional<std::string> reply = run->host->Call(*argv, run->resp, &error);
  if (!reply) {
    PushErrorTable(L, error);
    lua_pushboolean(L, 1);  // the stats were updated when the command ran (sl:944-948)
    lua_setfield(L, -2, "ignore_error_stats_update");
    return raise ? kRaise : 1;
  }
  Result<resp::ReplyNode> node = resp::ParseReply(*reply);
  if (!node.ok() || !PushReply(L, *node)) {
    spdlog::error("redis.call: cannot convert the reply of '{}'", argv->front());
    return PushInternalError(L);
  }
  return raise && node->type == resp::ReplyNode::Type::kError ? kRaise : 1;
}

int CallImpl(lua_State* L) { return GenericCommandImpl(L, true); }
int PCallImpl(lua_State* L) { return GenericCommandImpl(L, false); }

// redis.sha1hex (sl:1040-1059).
int Sha1HexImpl(lua_State* L) {
  if (lua_gettop(L) != 1) return PushLibError(L, kScriptErrWrongArgs);
  size_t len = 0;
  const char* s = lua_tolstring(L, 1, &len);  // a non-string gives nullptr and len 0
  const std::string digest = Sha1Hex(s == nullptr ? std::string_view() : std::string_view(s, len));
  lua_pushlstring(L, digest.data(), digest.size());
  return 1;
}

// redis.error_reply (sl:1078-1095). Bad arguments return, not raise, the error table.
int ErrorReplyImpl(lua_State* L) {
  if (lua_gettop(L) != 1 || lua_type(L, -1) != LUA_TSTRING) {
    PushErrorTable(L, kScriptErrWrongArgsOrType);
    return 1;
  }
  const std::string_view err(lua_tostring(L, -1));  // a C string in Redis: NUL truncates
  if (!err.empty() && err.front() == '-') {
    PushErrorTable(L, err);
  } else {
    std::string with_dash("-");
    with_dash += err;
    PushErrorTable(L, with_dash);
  }
  return 1;
}

// redis.status_reply (sl:1064-1076): {ok = <the argument>}.
int StatusReplyImpl(lua_State* L) {
  if (lua_gettop(L) != 1 || lua_type(L, -1) != LUA_TSTRING) {
    PushErrorTable(L, kScriptErrWrongArgsOrType);
    return 1;
  }
  lua_newtable(L);
  lua_pushvalue(L, -2);
  lua_setfield(L, -2, "ok");
  return 1;
}

// redis.log (sl:1166-1201): the host applies the verbosity.
int LogImpl(lua_State* L) {
  const int argc = lua_gettop(L);
  if (argc < 2) return PushLibError(L, kScriptErrLogArgs);
  if (!lua_isnumber(L, 1)) return PushLibError(L, kScriptErrLogLevelType);
  const int level = ToInt(L, 1);
  if (level < 0 || level > 3) return PushLibError(L, kScriptErrLogLevel);  // LL_DEBUG..LL_WARNING
  ScriptRun* run = CurrentRun(L);
  if (run == nullptr) return PushInternalError(L);
  std::string log;
  for (int j = 2; j <= argc; ++j) {
    size_t len = 0;
    const char* s = lua_tolstring(L, j, &len);  // numbers convert; other types are skipped
    if (s == nullptr) continue;
    if (j != 2) log += ' ';
    log.append(s, len);
  }
  run->host->Log(level, log);
  return 0;
}

// redis.setresp (sl:1203-1221).
int SetRespImpl(lua_State* L) {
  if (lua_gettop(L) != 1) return PushLibError(L, kScriptErrSetRespArgs);
  const int resp = ToInt(L, 1);
  if (resp != 2 && resp != 3) return PushLibError(L, kScriptErrSetRespVersion);
  ScriptRun* run = CurrentRun(L);
  if (run == nullptr) return PushInternalError(L);
  run->resp = resp;
  return 0;
}

// redis.set_repl (sl:1105-1128): validated, then a no-op (TidesKV has no replication).
int SetReplImpl(lua_State* L) {
  if (lua_gettop(L) != 1) return PushLibError(L, kScriptErrSetReplArgs);
  constexpr unsigned kReplAll = 3;  // PROPAGATE_AOF | PROPAGATE_REPL
  if ((static_cast<unsigned>(ToInt(L, -1)) & ~kReplAll) != 0) return PushLibError(L, kScriptErrSetReplFlags);
  return 0;
}

// redis.acl_check_cmd (sl:1129-1163). No ACLs yet: every existing command is permitted.
int AclCheckCmdImpl(lua_State* L) {
  std::string error;
  std::optional<std::vector<std::string>> argv = ArgsToArgv(L, &error);
  lua_settop(L, 0);
  if (!argv) return PushLibError(L, error);
  ScriptRun* run = CurrentRun(L);
  if (run == nullptr) return PushInternalError(L);
  if (!run->host->CommandExists(ToLower(argv->front()))) return PushLibError(L, kScriptErrAclCheckCmd);
  lua_pushboolean(L, 1);
  return 1;
}

}  // namespace

// The functions Lua calls. Each has only trivially destructible locals.
extern "C" {

static int LuaRedisCall(lua_State* L) noexcept { return RaiseIfAsked(L, Guarded(L, CallImpl)); }
static int LuaRedisPCall(lua_State* L) noexcept { return RaiseIfAsked(L, Guarded(L, PCallImpl)); }
static int LuaSha1Hex(lua_State* L) noexcept { return RaiseIfAsked(L, Guarded(L, Sha1HexImpl)); }
static int LuaErrorReply(lua_State* L) noexcept { return RaiseIfAsked(L, Guarded(L, ErrorReplyImpl)); }
static int LuaStatusReply(lua_State* L) noexcept { return RaiseIfAsked(L, Guarded(L, StatusReplyImpl)); }
static int LuaLog(lua_State* L) noexcept { return RaiseIfAsked(L, Guarded(L, LogImpl)); }
static int LuaSetResp(lua_State* L) noexcept { return RaiseIfAsked(L, Guarded(L, SetRespImpl)); }
static int LuaSetRepl(lua_State* L) noexcept { return RaiseIfAsked(L, Guarded(L, SetReplImpl)); }
static int LuaAclCheckCmd(lua_State* L) noexcept { return RaiseIfAsked(L, Guarded(L, AclCheckCmdImpl)); }

// luaRedisPcall (sl:1008-1027): pcall that unwraps {err = "..."} to its string.
static int LuaPcall(lua_State* L) noexcept {
  const int argc = lua_gettop(L);
  lua_pushboolean(L, 1);  // result placeholder
  lua_insert(L, 1);
  if (lua_pcall(L, argc - 1, LUA_MULTRET, 0) != 0) {
    lua_remove(L, 1);  // the placeholder
    if (lua_istable(L, -1)) {
      lua_getfield(L, -1, "err");
      if (lua_isstring(L, -1)) lua_replace(L, -2);  // the table becomes its message
    }
    lua_pushboolean(L, 0);
    lua_insert(L, 1);
  }
  return lua_gettop(L);
}

// redis.replicate_commands (ev:168-171), redis.breakpoint and redis.debug without LDB (ev:134-159).
static int LuaReplicateCommands(lua_State* L) noexcept {
  lua_pushboolean(L, 1);
  return 1;
}
static int LuaBreakpoint(lua_State* L) noexcept {
  lua_pushboolean(L, 0);
  return 1;
}
static int LuaDebug(lua_State* /*L*/) noexcept { return 0; }

// redis_math_random (sl:1576-1601): Lua 5.1's math.random over Redis's rand48 stream.
static int LuaMathRandom(lua_State* L) noexcept {
  const lua_Number r =
      static_cast<lua_Number>(RedisLrand48() % kRedisLrand48Max) / static_cast<lua_Number>(kRedisLrand48Max);
  switch (lua_gettop(L)) {
    case 0:
      lua_pushnumber(L, r);
      break;
    case 1: {
      const int u = luaL_checkint(L, 1);
      luaL_argcheck(L, 1 <= u, 1, "interval is empty");
      lua_pushnumber(L, std::floor(r * u) + 1);
      break;
    }
    case 2: {
      const int l = luaL_checkint(L, 1);
      const int u = luaL_checkint(L, 2);
      luaL_argcheck(L, l <= u, 2, "interval is empty");
      // In int64_t: Redis's int `u - l + 1` overflows (UB) for ranges wider than INT_MAX.
      lua_pushnumber(L, std::floor(r * static_cast<lua_Number>(int64_t{u} - l + 1)) + l);
      break;
    }
    default:  // luaL_error: prefixed with luaL_where(L, 1)
      luaL_where(L, 1);
      lua_pushlstring(L, kScriptErrWrongArgs.data(), kScriptErrWrongArgs.size());
      lua_concat(L, 2);
      return lua_error(L);
  }
  return 1;
}

static int LuaMathRandomseed(lua_State* L) noexcept {
  RedisSrand48(luaL_checkint(L, 1));
  return 0;
}

}  // extern "C"

namespace {

void SetFunction(lua_State* L, const char* name, lua_CFunction f) {
  lua_pushcfunction(L, f);
  lua_setfield(L, -2, name);
}

void SetNumber(lua_State* L, const char* name, lua_Number n) {
  lua_pushnumber(L, n);
  lua_setfield(L, -2, name);
}

// The `redis` table in Redis's insertion order (sl:1462-1530, then ev:198-218).
void PushRedisTable(lua_State* L) {
  lua_newtable(L);
  SetFunction(L, "call", LuaRedisCall);
  SetFunction(L, "pcall", LuaRedisPCall);
  SetFunction(L, "log", LuaLog);
  SetNumber(L, "LOG_DEBUG", 0);
  SetNumber(L, "LOG_VERBOSE", 1);
  SetNumber(L, "LOG_NOTICE", 2);
  SetNumber(L, "LOG_WARNING", 3);
  SetNumber(L, "REDIS_VERSION_NUM", RedisVersionNum(kRedisCompatVersion));
  lua_pushlstring(L, kRedisCompatVersion.data(), kRedisCompatVersion.size());
  lua_setfield(L, -2, "REDIS_VERSION");
  SetFunction(L, "setresp", LuaSetResp);
  SetFunction(L, "sha1hex", LuaSha1Hex);
  SetFunction(L, "error_reply", LuaErrorReply);
  SetFunction(L, "status_reply", LuaStatusReply);
  SetFunction(L, "set_repl", LuaSetRepl);
  SetNumber(L, "REPL_NONE", 0);
  SetNumber(L, "REPL_AOF", 1);
  SetNumber(L, "REPL_SLAVE", 2);
  SetNumber(L, "REPL_REPLICA", 2);
  SetNumber(L, "REPL_ALL", 3);
  SetFunction(L, "acl_check_cmd", LuaAclCheckCmd);
  SetFunction(L, "breakpoint", LuaBreakpoint);
  SetFunction(L, "debug", LuaDebug);
  SetFunction(L, "replicate_commands", LuaReplicateCommands);
}

void PushRunKey(lua_State* L) { lua_pushlightuserdata(L, const_cast<char*>(&kRunKey)); }

}  // namespace

void RegisterRedisApi(lua_State* L) {
  lua_pushcfunction(L, LuaPcall);
  lua_setglobal(L, "pcall");
  PushRedisTable(L);
  lua_setglobal(L, "redis");
  lua_getglobal(L, "math");
  SetFunction(L, "random", LuaMathRandom);
  SetFunction(L, "randomseed", LuaMathRandomseed);
  lua_pop(L, 1);
}

ScriptRun* CurrentRun(lua_State* L) {
  PushRunKey(L);
  lua_rawget(L, LUA_REGISTRYINDEX);
  auto* run = static_cast<ScriptRun*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return run;
}

void SetCurrentRun(lua_State* L, ScriptRun* run) {
  PushRunKey(L);
  if (run != nullptr) {
    lua_pushlightuserdata(L, run);
  } else {
    lua_pushnil(L);
  }
  lua_rawset(L, LUA_REGISTRYINDEX);
}

}  // namespace tideskv

// luaMaskCountHook (sl:1610-1628). No C++ object is alive at lua_error.
extern "C" void ScriptCountHook(lua_State* L, lua_Debug* /*ar*/) noexcept {
  tideskv::ScriptRun* run = tideskv::CurrentRun(L);
  if (run == nullptr || (!run->killed && !run->host->ShouldKill())) return;  // sticky once killed
  run->killed = true;
  spdlog::info("Lua script killed by user with SCRIPT KILL.");
  // Fire on every line from now on, so the script cannot catch the error with pcall and loop.
  lua_sethook(L, ScriptCountHook, LUA_MASKLINE, 0);
  tideskv::PushErrorTable(L, tideskv::kScriptErrKilled);
  lua_error(L);
}
