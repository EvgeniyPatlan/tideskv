#include "script/lua_sandbox.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstdlib>

#include "script/lua_api.h"
#include "script/lua_convert.h"
#include "script/redis_api.h"
#include "types/errors.h"

// Port of Redis 7.2.16 eval.c scriptingInit/luaCreateFunction and script_lua.c (sl) sandboxing
// and luaCallFunction. Functions run by Lua (lua_CFunction or under lua_cpcall) have no C++ object
// with a destructor alive when Lua may raise (lua-integration.md §6).

namespace tideskv {
namespace {

// The globals allowed while the state is built (sl:46-115): libraries, the Redis API, the Lua
// builtins and `debug`, which the error-handler prelude removes again. The deprecated builtins
// (sl:94-99) are never allowed (M5b H9: lua-enable-deprecated-api is not supported).
constexpr std::array<std::string_view, 34> kAllowedGlobals = {"string",
                                                              "cjson",
                                                              "bit",
                                                              "cmsgpack",
                                                              "math",
                                                              "table",
                                                              "struct",
                                                              "redis",
                                                              "__redis__err__handler",
                                                              "xpcall",
                                                              "tostring",
                                                              "setmetatable",
                                                              "next",
                                                              "assert",
                                                              "tonumber",
                                                              "rawequal",
                                                              "collectgarbage",
                                                              "getmetatable",
                                                              "rawset",
                                                              "pcall",
                                                              "coroutine",
                                                              "type",
                                                              "_G",
                                                              "select",
                                                              "unpack",
                                                              "gcinfo",
                                                              "pairs",
                                                              "rawget",
                                                              "loadstring",
                                                              "ipairs",
                                                              "_VERSION",
                                                              "load",
                                                              "error",
                                                              "debug"};
constexpr std::array<std::string_view, 3> kDeprecatedGlobals = {"newproxy", "setfenv", "getfenv"};
constexpr std::array<std::string_view, 3> kDeniedGlobals = {"dofile", "loadfile", "print"};

bool Contains(const auto& list, std::string_view name) {
  return std::find(list.begin(), list.end(), name) != list.end();
}

// The error handler Redis defines in Lua (eval.c:225-242), byte for byte.
constexpr char kErrHandler[] =
    "local dbg = debug\n"
    "debug = nil\n"
    "function __redis__err__handler(err)\n"
    "  local i = dbg.getinfo(2,'nSl')\n"
    "  if i and i.what == 'C' then\n"
    "    i = dbg.getinfo(3,'nSl')\n"
    "  end\n"
    "  if type(err) ~= 'table' then\n"
    "    err = {err='ERR ' .. tostring(err)}"
    "  end"
    "  if i then\n"
    "    err['source'] = i.source\n"
    "    err['line'] = i.currentline\n"
    "  end"
    "  return err\n"
    "end\n";

constexpr int kGcCyclePeriod = 50;  // LUA_GC_CYCLE_PERIOD (sl:1680)

// luaL_error from a C function: the message has no position prefix.
int RaiseMessage(lua_State* L, std::string_view msg) {
  lua_pushlstring(L, msg.data(), msg.size());
  return lua_error(L);
}

struct CompileJob {
  std::string name;  // "f_<sha>", built before lua_cpcall
  std::string_view code;
  std::string error;
  int rc = 0;
};

struct ArraysJob {
  const std::vector<std::string>* keys;
  const std::vector<std::string>* args;
};

struct ErrorInfo {  // luaExtractErrorInformation (sl:1634-1667)
  std::string msg;
  std::string source;
  std::string line;
  bool has_source = false;
  bool has_line = false;
};

}  // namespace

extern "C" {

static int Panic(lua_State* L) noexcept {
  const char* msg = lua_tostring(L, -1);
  spdlog::critical("Lua panic: {}", msg != nullptr ? msg : "?");
  std::abort();
}

// luaProtectedTableError (sl:1267-1282): the __index of _G.
static int ProtectedTableError(lua_State* L) noexcept {
  if (lua_gettop(L) != 2) {
    spdlog::warn("malicious code trying to call luaProtectedTableError with wrong arguments");
    return RaiseMessage(L, kScriptErrProtectedTableArgs);
  }
  if (!lua_isstring(L, -1) && !lua_isnumber(L, -1)) return RaiseMessage(L, kScriptErrProtectedTableKey);
  const char* name = lua_tostring(L, -1);
  lua_pushlstring(L, kScriptErrNonexistentGlobal.data(), kScriptErrNonexistentGlobal.size());
  lua_pushstring(L, name);
  lua_pushliteral(L, "'");
  lua_concat(L, 3);
  return lua_error(L);
}

// luaNewIndexAllowList (sl:1295-1350): the __newindex of _G while the state is built.
static int NewIndexAllowList(lua_State* L) noexcept {
  if (lua_gettop(L) != 3) {
    spdlog::warn("malicious code trying to call luaNewIndexAllowList with wrong arguments");
    return RaiseMessage(L, kScriptErrAllowListArgs);
  }
  if (!lua_istable(L, -3)) return RaiseMessage(L, kScriptErrAllowListTable);
  if (!lua_isstring(L, -2) && !lua_isnumber(L, -2)) return RaiseMessage(L, kScriptErrAllowListKey);
  const std::string_view name(lua_tostring(L, -2));
  if (Contains(kAllowedGlobals, name)) {
    lua_rawset(L, -3);
  } else if (!Contains(kDeniedGlobals, name) && !Contains(kDeprecatedGlobals, name)) {
    spdlog::warn(
        "A key '{}' was added to Lua globals which is not on the globals allow list nor listed "
        "on the deny list.",
        name);
  }
  return 0;
}

}  // extern "C"

namespace {

void SetAllowListProtection(lua_State* L) {  // sl:1361
  lua_newtable(L);
  lua_pushcfunction(L, NewIndexAllowList);
  lua_setfield(L, -2, "__newindex");
  lua_setmetatable(L, -2);
}

void SetErrorMetatable(lua_State* L) {  // sl:1284-1293
  lua_newtable(L);
  lua_pushcfunction(L, ProtectedTableError);
  lua_setfield(L, -2, "__index");
  lua_setmetatable(L, -2);
}

// sl:1371-1397: the table on top, every table reachable from it and their metatables.
void SetTableProtectionRecursively(lua_State* L) {
  if (lua_isreadonlytable(L, -1)) return;  // visited (_G._G)
  lua_enablereadonlytable(L, -1, 1);
  lua_checkstack(L, 2);
  lua_pushnil(L);
  while (lua_next(L, -2)) {
    if (lua_istable(L, -1)) SetTableProtectionRecursively(L);
    lua_pop(L, 1);
  }
  if (lua_getmetatable(L, -1)) {
    SetTableProtectionRecursively(L);
    lua_pop(L, 1);
  }
}

// sl:1399-1427 (CVE-2025-46818): the metatables of the basic types, such as string's.
void SetTableProtectionForBasicTypes(lua_State* L) {
  for (const int type :
       {LUA_TSTRING, LUA_TNUMBER, LUA_TBOOLEAN, LUA_TNIL, LUA_TFUNCTION, LUA_TTHREAD, LUA_TLIGHTUSERDATA}) {
    switch (type) {
      case LUA_TSTRING:
        lua_pushstring(L, "");
        break;
      case LUA_TNUMBER:
        lua_pushnumber(L, 0);
        break;
      case LUA_TBOOLEAN:
        lua_pushboolean(L, 0);
        break;
      case LUA_TNIL:
        lua_pushnil(L);
        break;
      case LUA_TFUNCTION:
        lua_pushcfunction(L, nullptr);
        break;
      case LUA_TTHREAD:
        lua_newthread(L);
        break;
      default:
        lua_pushlightuserdata(L, L);
        break;
    }
    if (lua_getmetatable(L, -1)) {
      SetTableProtectionRecursively(L);
      lua_pop(L, 1);
    }
    lua_pop(L, 1);
  }
}

void LoadLibrary(lua_State* L, const char* name, lua_CFunction open) {  // luaLoadLib (sl:1225)
  lua_pushcfunction(L, open);
  lua_pushstring(L, name);
  lua_call(L, 1, 0);
}

void PushArray(lua_State* L, const std::vector<std::string>& elems) {  // luaCreateArray
  lua_newtable(L);
  for (size_t j = 0; j < elems.size(); ++j) {
    lua_pushlstring(L, elems[j].data(), elems[j].size());
    lua_rawseti(L, -2, static_cast<int>(j + 1));
  }
}

void ReadErrorField(lua_State* L, const char* field, std::string* out, bool* found) {
  lua_getfield(L, 1, field);
  if (lua_isstring(L, -1)) {
    *out = lua_tostring(L, -1);  // a C string in Redis (sdsnew): NUL truncates
    *found = true;
  }
  lua_pop(L, 1);
}

}  // namespace

extern "C" {

// eval.c scriptingInit + script_lua.c luaRegisterRedisAPI, run under lua_cpcall.
static int InitState(lua_State* L) noexcept {
  lua_pushvalue(L, LUA_GLOBALSINDEX);
  SetAllowListProtection(L);
  lua_pop(L, 1);
  LoadLibrary(L, "", luaopen_base);  // luaLoadLibraries (sl:1237-1253)
  LoadLibrary(L, LUA_TABLIBNAME, luaopen_table);
  LoadLibrary(L, LUA_STRLIBNAME, luaopen_string);
  LoadLibrary(L, LUA_MATHLIBNAME, luaopen_math);
  LoadLibrary(L, LUA_DBLIBNAME, luaopen_debug);
  LoadLibrary(L, "cjson", luaopen_cjson);
  LoadLibrary(L, "struct", luaopen_struct);
  LoadLibrary(L, "cmsgpack", luaopen_cmsgpack);
  LoadLibrary(L, "bit", luaopen_bit);
  RegisterRedisApi(L);
  if (luaL_loadbuffer(L, kErrHandler, sizeof(kErrHandler) - 1, "@err_handler_def") != 0) return lua_error(L);
  lua_call(L, 0, 0);
  lua_pushvalue(L, LUA_GLOBALSINDEX);  // lock _G and everything reachable from it (ev:256-262)
  SetErrorMetatable(L);
  SetTableProtectionRecursively(L);
  lua_pop(L, 1);
  SetTableProtectionForBasicTypes(L);
  return 0;
}

static int CompileInLua(lua_State* L) noexcept {  // luaCreateFunction (ev:444-460)
  auto* job = static_cast<CompileJob*>(lua_touserdata(L, 1));
  job->rc = luaL_loadbuffer(L, job->code.data(), job->code.size(), "@user_script");
  if (job->rc != 0) {
    const char* msg = lua_tostring(L, -1);
    job->error = msg != nullptr ? msg : "";
    lua_pop(L, 1);
    return 0;
  }
  lua_setfield(L, LUA_REGISTRYINDEX, job->name.c_str());
  return 0;
}

static int SetKeysAndArgvInLua(lua_State* L) noexcept {  // sl:1644-1658, EVAL mode
  const auto* job = static_cast<const ArraysJob*>(lua_touserdata(L, 1));
  PushArray(L, *job->keys);
  lua_enablereadonlytable(L, LUA_GLOBALSINDEX, 0);
  lua_setglobal(L, "KEYS");
  lua_enablereadonlytable(L, LUA_GLOBALSINDEX, 1);
  PushArray(L, *job->args);
  lua_enablereadonlytable(L, LUA_GLOBALSINDEX, 0);
  lua_setglobal(L, "ARGV");
  lua_enablereadonlytable(L, LUA_GLOBALSINDEX, 1);
  return 0;
}

// Called as f(error_table, info): the fields are read with lua_getfield, as Redis does, but under
// lua_pcall, because a metatable on a table passed to error() can make the read raise.
static int ExtractErrorInfo(lua_State* L) noexcept {
  auto* info = static_cast<ErrorInfo*>(lua_touserdata(L, 2));
  bool has_msg = false;
  ReadErrorField(L, "err", &info->msg, &has_msg);
  ReadErrorField(L, "source", &info->source, &info->has_source);
  ReadErrorField(L, "line", &info->line, &info->has_line);
  return 0;
}

}  // extern "C"

namespace {

// sdstrim(s, "\r\n") at the end, as addReplyErrorFormat does.
void TrimTrailingCrLf(std::string* s) {
  while (!s->empty() && (s->back() == '\r' || s->back() == '\n')) s->pop_back();
}

// A non-table error object, or one whose fields could not be read (sl:1690-1699).
void WriteRunFailure(lua_State* L, const std::string& sha, resp::ReplyBuilder& out) {
  const char* msg = lua_isstring(L, -1) ? lua_tostring(L, -1) : "execution failure";
  std::string text = "ERR Error running script " + sha + ", ";
  text.append(std::string_view(msg).substr(0, 100));  // %.100s
  TrimTrailingCrLf(&text);
  out.Error(text);
}

// Writes the error object on top of the stack and pops it (sl:1690-1715).
void WriteScriptError(lua_State* L, const std::string& sha, resp::ReplyBuilder& out) {
  if (!lua_istable(L, -1)) {
    WriteRunFailure(L, sha, out);
    lua_pop(L, 1);
    return;
  }
  ErrorInfo info;
  lua_pushcfunction(L, ExtractErrorInfo);
  lua_pushvalue(L, -2);
  lua_pushlightuserdata(L, &info);
  if (lua_pcall(L, 2, 0, 0) != 0) {
    WriteRunFailure(L, sha, out);
    lua_pop(L, 2);  // its error, then the error table
    return;
  }
  std::string text = std::move(info.msg);  // Redis would crash without `err`; TidesKV sends "-"
  if (info.has_source && info.has_line) {
    text += " script: " + sha + ", on " + info.source + ":" + info.line + ".";
  }
  out.Error(text);
  lua_pop(L, 1);
}

}  // namespace

std::unique_ptr<LuaSandbox> LuaSandbox::Create() {
  lua_State* L = luaL_newstate();
  if (L == nullptr) {
    spdlog::critical("cannot create the Lua state");
    std::abort();
  }
  lua_atpanic(L, Panic);
  if (lua_cpcall(L, InitState, nullptr) != 0) {
    const char* msg = lua_tostring(L, -1);
    spdlog::critical("cannot initialise Lua: {}", msg != nullptr ? msg : "?");
    std::abort();
  }
  return std::unique_ptr<LuaSandbox>(new LuaSandbox(L));
}

LuaSandbox::~LuaSandbox() {
  lua_gc(L_, LUA_GCCOLLECT, 0);  // before lua_close (CVE-2024-46981)
  lua_close(L_);
}

Status LuaSandbox::Compile(const std::string& sha, std::string_view code) {
  CompileJob job{"f_" + sha, code, {}, 0};
  if (lua_cpcall(L_, CompileInLua, &job) != 0) {
    lua_pop(L_, 1);
    return Status::Internal("cannot store the compiled script");
  }
  if (job.rc != 0) return Status::UserError(std::string(kErrCompilingScript) + job.error);
  return Status::Ok();
}

// luaCallFunction (sl:1669-1772) in EVAL mode.
LuaSandbox::RunResult LuaSandbox::Run(const std::string& sha, const std::vector<std::string>& keys,
                                      const std::vector<std::string>& args, SandboxHost& host,
                                      const RunOptions& opts, resp::ReplyBuilder& out) {
  ScriptRun run{&host, 2, false};
  lua_State* L = L_;
  lua_getglobal(L, "__redis__err__handler");                 // [handler]
  lua_getfield(L, LUA_REGISTRYINDEX, ("f_" + sha).c_str());  // [handler, f]
  SetCurrentRun(L, &run);
  if (opts.hook) lua_sethook(L, ScriptCountHook, LUA_MASKCOUNT, 100000);
  ArraysJob arrays{&keys, &args};
  int err = lua_cpcall(L, SetKeysAndArgvInLua, &arrays);
  if (err == 0) {
    err = lua_pcall(L, 0, 1, -2);
  } else {
    lua_remove(L, -2);  // f: the cpcall error takes its place
  }
  if (++gc_runs_ == kGcCyclePeriod) {
    lua_gc(L, LUA_GCSTEP, kGcCyclePeriod);
    gc_runs_ = 0;
  }
  if (err != 0) {
    WriteScriptError(L, sha, out);  // consumes the error object
  } else {
    WriteLuaValue(L, run.resp, out);  // consumes the result
  }
  if (opts.hook) lua_sethook(L, nullptr, 0, 0);
  SetCurrentRun(L, nullptr);
  lua_pop(L, 1);  // the error handler
  return {run.killed};
}

size_t LuaSandbox::memory_bytes() const { return static_cast<size_t>(lua_gc(L_, LUA_GCCOUNT, 0)) * 1024; }

}  // namespace tideskv
