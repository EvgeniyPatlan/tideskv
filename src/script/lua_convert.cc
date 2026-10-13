#include "script/lua_convert.h"

#include <cmath>
#include <limits>

#include "script/lua_api.h"
#include "types/errors.h"

extern "C" {
#include "fpconv_dtoa.h"
}

namespace tideskv {
namespace {

using Type = resp::ReplyNode::Type;

void SetStringField(lua_State* L, const char* field, std::string_view value) {
  lua_pushlstring(L, value.data(), value.size());
  lua_setfield(L, -2, field);
}

// Wraps the value on top of the stack as {field = value}.
void WrapField(lua_State* L, const char* field) {
  lua_newtable(L);
  lua_insert(L, -2);
  lua_setfield(L, -2, field);
}

std::string MapCrLf(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c == '\r' || c == '\n') c = ' ';
  }
  return out;
}

bool PushAggregate(lua_State* L, const resp::ReplyNode& node) {
  lua_newtable(L);
  if (node.type == Type::kArray) {
    for (size_t i = 0; i < node.elems.size(); ++i) {
      if (!PushReply(L, node.elems[i])) return lua_pop(L, 1), false;
      lua_rawseti(L, -2, static_cast<int>(i + 1));  // a nil element leaves a hole (sl:474)
    }
    return true;
  }
  const bool is_map = node.type == Type::kMap;
  for (size_t i = 0; i < node.elems.size(); i += is_map ? 2 : 1) {
    if (!PushReply(L, node.elems[i])) return lua_pop(L, 1), false;
    if (is_map) {
      if (!PushReply(L, node.elems[i + 1])) return lua_pop(L, 2), false;
    } else {
      lua_pushboolean(L, 1);
    }
    lua_settable(L, -3);
  }
  WrapField(L, is_map ? "map" : "set");
  return true;
}

// Pushes t[name] (raw) and returns its Lua type.
int RawField(lua_State* L, int t, const char* name) {
  lua_pushstring(L, name);
  lua_rawget(L, t);
  return lua_type(L, -1);
}

std::string_view ToView(lua_State* L, int idx) {
  size_t n = 0;
  const char* s = lua_tolstring(L, idx, &n);
  return std::string_view(s, n);
}

// verbatim_string = {format=, string=}; the table is at `v`.
bool WriteVerbatim(lua_State* L, int v, resp::ReplyBuilder& out) {
  bool done = false;
  if (RawField(L, v, "format") == LUA_TSTRING) {
    if (RawField(L, v, "string") == LUA_TSTRING) {
      out.Verbatim(ToView(L, -1), ToView(L, -2));
      done = true;
    }
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  return done;
}

// err / ok / double / big_number / verbatim_string. Leaves the stack as it found it.
bool WriteScalarField(lua_State* L, int t, resp::ReplyBuilder& out) {
  if (RawField(L, t, "err") == LUA_TSTRING) {
    out.Error(ToView(L, -1));
    lua_pop(L, 1);
    return true;
  }
  lua_pop(L, 1);
  if (RawField(L, t, "ok") == LUA_TSTRING) {
    out.SimpleString(ToView(L, -1));
    lua_pop(L, 1);
    return true;
  }
  lua_pop(L, 1);
  if (RawField(L, t, "double") == LUA_TNUMBER) {
    out.Double(lua_tonumber(L, -1));
    lua_pop(L, 1);
    return true;
  }
  lua_pop(L, 1);
  if (RawField(L, t, "big_number") == LUA_TSTRING) {
    out.BigNumber(MapCrLf(ToView(L, -1)));
    lua_pop(L, 1);
    return true;
  }
  lua_pop(L, 1);
  if (RawField(L, t, "verbatim_string") == LUA_TTABLE && WriteVerbatim(L, lua_gettop(L), out)) {
    lua_pop(L, 1);
    return true;
  }
  lua_pop(L, 1);
  return false;
}

// Aggregates are written into a child builder and counted, then emitted after their header:
// ReplyBuilder has no deferred lengths (Redis uses addReplyDeferredLen).
void WriteMap(lua_State* L, int script_resp, resp::ReplyBuilder& out) {
  resp::ReplyBuilder child(out.protocol());
  size_t n = 0;
  lua_pushnil(L);
  while (lua_next(L, -2)) {
    lua_pushvalue(L, -2);  // dup the key before it is consumed
    WriteLuaValue(L, script_resp, child);
    WriteLuaValue(L, script_resp, child);
    ++n;
  }
  out.MapHeader(n);
  out.Raw(child.buffer());
}

void WriteSet(lua_State* L, int script_resp, resp::ReplyBuilder& out) {
  resp::ReplyBuilder child(out.protocol());
  size_t n = 0;
  lua_pushnil(L);
  while (lua_next(L, -2)) {
    lua_pop(L, 1);  // the boolean value
    lua_pushvalue(L, -1);
    WriteLuaValue(L, script_resp, child);
    ++n;
  }
  out.SetHeader(n);
  out.Raw(child.buffer());
}

void WriteArray(lua_State* L, int script_resp, resp::ReplyBuilder& out) {
  resp::ReplyBuilder child(out.protocol());
  size_t n = 0;
  for (int j = 1;; ++j) {
    lua_rawgeti(L, -1, j);
    if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      break;
    }
    WriteLuaValue(L, script_resp, child);
    ++n;
  }
  out.ArrayHeader(n);
  out.Raw(child.buffer());
}

// Follows luaReplyToRedisReply's table cases (sl:625-776). Leaves the table on the stack.
void WriteTypedTable(lua_State* L, int script_resp, resp::ReplyBuilder& out) {
  const int t = lua_gettop(L);
  if (WriteScalarField(L, t, out)) return;
  if (RawField(L, t, "map") == LUA_TTABLE) {
    WriteMap(L, script_resp, out);
    lua_pop(L, 1);
    return;
  }
  lua_pop(L, 1);
  if (RawField(L, t, "set") == LUA_TTABLE) {
    WriteSet(L, script_resp, out);
    lua_pop(L, 1);
    return;
  }
  lua_pop(L, 1);
  WriteArray(L, script_resp, out);
}

}  // namespace

int64_t LuaNumberToInteger(double d) {
  if (!(d >= -0x1p63 && d < 0x1p63)) return std::numeric_limits<int64_t>::min();
  return static_cast<int64_t>(d);  // truncation toward zero, like Redis's (long long) cast
}

void PushErrorTable(lua_State* L, std::string_view error) {
  std::string_view code = "ERR";
  std::string_view msg = error;
  if (!error.empty() && error[0] == '-') {
    const size_t space = error.find(' ');
    if (space == std::string_view::npos) {
      msg = error.substr(1);
    } else {
      code = error.substr(1, space - 1);
      msg = error.substr(space + 1);
    }
  }
  while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n')) msg.remove_suffix(1);
  while (!msg.empty() && (msg.front() == '\r' || msg.front() == '\n')) msg.remove_prefix(1);
  std::string text(code);
  text += ' ';
  text.append(msg);
  lua_newtable(L);
  SetStringField(L, "err", text);
}

bool PushReply(lua_State* L, const resp::ReplyNode& node) {
  if (!lua_checkstack(L, 4)) return false;
  switch (node.type) {
    case Type::kInteger:
      lua_pushnumber(L, static_cast<lua_Number>(node.integer));
      return true;
    case Type::kBulk:
      lua_pushlstring(L, node.str.data(), node.str.size());
      return true;
    case Type::kNullBulk:
    case Type::kNullArray:
      lua_pushboolean(L, 0);
      return true;
    case Type::kNull:
      lua_pushnil(L);
      return true;
    case Type::kBool:
      lua_pushboolean(L, node.boolean ? 1 : 0);
      return true;
    case Type::kStatus:
      lua_newtable(L);
      SetStringField(L, "ok", node.str);
      return true;
    case Type::kError:
      PushErrorTable(L, "-" + node.str);
      lua_pushboolean(L, 1);
      lua_setfield(L, -2, "ignore_error_stats_update");
      return true;
    case Type::kDouble:
      lua_pushnumber(L, node.dbl);
      WrapField(L, "double");
      return true;
    case Type::kBigNumber:
      lua_pushlstring(L, node.str.data(), node.str.size());
      WrapField(L, "big_number");
      return true;
    case Type::kVerbatim:
      lua_newtable(L);
      SetStringField(L, "string", node.str);
      SetStringField(L, "format", node.format);
      WrapField(L, "verbatim_string");
      return true;
    default:
      return PushAggregate(L, node);
  }
}

void WriteLuaValue(lua_State* L, int script_resp, resp::ReplyBuilder& out) {
  if (!lua_checkstack(L, 4)) {  // sl:600-608: the innermost element becomes this error
    out.Error("ERR reached lua stack limit");
    lua_pop(L, 1);
    return;
  }
  switch (lua_type(L, -1)) {
    case LUA_TSTRING:
      out.Bulk(ToView(L, -1));
      break;
    case LUA_TBOOLEAN:
      if (script_resp == 2) {
        lua_toboolean(L, -1) ? out.Integer(1) : out.Null();
      } else {
        out.Boolean(lua_toboolean(L, -1) != 0);
      }
      break;
    case LUA_TNUMBER:
      out.Integer(LuaNumberToInteger(lua_tonumber(L, -1)));
      break;
    case LUA_TTABLE:
      WriteTypedTable(L, script_resp, out);
      break;
    default:
      out.Null();
  }
  lua_pop(L, 1);
}

std::optional<std::vector<std::string>> ArgsToArgv(lua_State* L, std::string* error) {
  const int argc = lua_gettop(L);
  if (argc == 0) {
    *error = std::string(kScriptErrNoArgs);
    return std::nullopt;
  }
  std::vector<std::string> argv;
  argv.reserve(static_cast<size_t>(argc));
  for (int j = 1; j <= argc; ++j) {
    if (lua_type(L, j) == LUA_TNUMBER) {  // lua_tolstring would lose precision (%.14g)
      const double d = lua_tonumber(L, j);
      constexpr double kHalf = 0x1p62;  // LLONG_MAX/2 rounded to double, as in util.h double2ll
      if (d >= -kHalf && d <= kHalf && static_cast<double>(static_cast<int64_t>(d)) == d) {
        argv.push_back(std::to_string(static_cast<int64_t>(d)));  // util.h double2ll
      } else {
        char buf[24];
        argv.emplace_back(buf, static_cast<size_t>(fpconv_dtoa(d, buf)));
      }
    } else if (lua_type(L, j) == LUA_TSTRING) {
      argv.emplace_back(ToView(L, j));
    } else {
      *error = std::string(kScriptErrArgType);
      return std::nullopt;
    }
  }
  return argv;
}

}  // namespace tideskv
