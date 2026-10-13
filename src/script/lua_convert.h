#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "resp/reply_builder.h"
#include "resp/reply_parser.h"

struct lua_State;

namespace tideskv {

// (long long)d without UB: truncation, INT64_MIN when out of range or NaN.
int64_t LuaNumberToInteger(double d);

// redisProtocolToLuaType: pushes `node` as a Lua value. False when the Lua stack cannot grow.
bool PushReply(lua_State* L, const resp::ReplyNode& node);

// luaPushErrorBuff: pushes {err = "<CODE> <message>"} built from `error` (optionally "-CODE msg").
void PushErrorTable(lua_State* L, std::string_view error);

// luaReplyToRedisReply: writes the value on top of the stack as a reply and pops it.
void WriteLuaValue(lua_State* L, int script_resp, resp::ReplyBuilder& out);

// luaArgsToRedisArgv: converts all stack arguments; nullopt and *error when invalid.
std::optional<std::vector<std::string>> ArgsToArgv(lua_State* L, std::string* error);

}  // namespace tideskv
