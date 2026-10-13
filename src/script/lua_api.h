#pragma once
// The only way C++ includes Lua (M5b plan). Lua 5.1's headers have no extern "C".
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

// Redis-bundled libraries (third_party/lua); Redis declares these in script_lua.c.
int luaopen_cjson(lua_State* L);
int luaopen_struct(lua_State* L);
int luaopen_cmsgpack(lua_State* L);
int luaopen_bit(lua_State* L);
}
