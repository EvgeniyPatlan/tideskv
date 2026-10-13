#include <gtest/gtest.h>

#include <string>

#include "script/lua_api.h"

namespace tideskv {
namespace {

struct Lua {
  lua_State* L = luaL_newstate();
  Lua() {
    for (auto [name, fn] : {std::pair<const char*, lua_CFunction>{"", luaopen_base},
                            {"string", luaopen_string},
                            {"table", luaopen_table},
                            {"cjson", luaopen_cjson},
                            {"bit", luaopen_bit},
                            {"cmsgpack", luaopen_cmsgpack},
                            {"struct", luaopen_struct}}) {
      lua_pushcfunction(L, fn);
      lua_pushstring(L, name);
      lua_call(L, 1, 0);
    }
  }
  ~Lua() { lua_close(L); }
  // Runs `code`; returns tostring(result) or "ERR:" + the error message.
  std::string Eval(const std::string& code) {
    if (luaL_loadbuffer(L, code.data(), code.size(), "@user_script") != 0 || lua_pcall(L, 0, 1, 0) != 0) {
      std::string e = "ERR:" + std::string(lua_tostring(L, -1));
      lua_pop(L, 1);
      return e;
    }
    lua_getglobal(L, "tostring");
    lua_insert(L, -2);
    lua_call(L, 1, 1);
    std::string r = lua_tostring(L, -1);
    lua_pop(L, 1);
    return r;
  }
};

TEST(LuaVendor, RunsCode) {
  Lua l;
  EXPECT_EQ(l.Eval("return 1+1"), "2");
  EXPECT_EQ(l.Eval("return 0.1+0.2"), "0.3");  // %.14g
  EXPECT_EQ(l.Eval("return cjson.encode({1,2,'x'})"), "[1,2,\"x\"]");
  EXPECT_EQ(l.Eval("return bit.tohex(255, 4)"), "00ff");
  EXPECT_EQ(l.Eval("return bit.tohex(1, -2147483648)"), "00000001");  // CVE-2024-31449: 7.2.5 crashed
  EXPECT_EQ(l.Eval("return struct.pack('>I2', 258):byte(1)"), "1");
  EXPECT_EQ(l.Eval("return #cmsgpack.pack({1,2,3})"), "4");
}

TEST(LuaVendor, ReadonlyTablePatch) {
  Lua l;
  lua_newtable(l.L);
  lua_enablereadonlytable(l.L, -1, 1);
  lua_setglobal(l.L, "t");
  EXPECT_EQ(l.Eval("t.x = 1"), "ERR:user_script:1: Attempt to modify a readonly table");
}

TEST(LuaVendor, BytecodeIsRejected) {
  Lua l;
  const std::string r = l.Eval("return loadstring(string.dump(function() return 1 end))");
  EXPECT_EQ(r, "nil");  // the patched parser never undumps: loadstring fails and returns nil
}

}  // namespace
}  // namespace tideskv
