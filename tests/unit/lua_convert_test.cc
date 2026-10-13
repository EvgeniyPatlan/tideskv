#include "script/lua_convert.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "resp/reply_parser.h"
#include "script/lua_api.h"

namespace tideskv {
namespace {

struct Bare {
  lua_State* L = luaL_newstate();
  Bare() {
    for (lua_CFunction fn : {luaopen_base, luaopen_table, luaopen_string}) {
      lua_pushcfunction(L, fn);
      lua_pushstring(L, "");
      lua_call(L, 1, 0);
    }
  }
  ~Bare() { lua_close(L); }
  void Push(const std::string& expr) {
    const std::string code = "return " + expr;
    ASSERT_EQ(luaL_loadbuffer(L, code.data(), code.size(), "@t"), 0);
    ASSERT_EQ(lua_pcall(L, 0, 1, 0), 0) << lua_tostring(L, -1);
  }
  std::string Reply(const std::string& expr, int script_resp = 2, int caller_resp = 2) {
    Push(expr);
    resp::ReplyBuilder out(caller_resp);
    WriteLuaValue(L, script_resp, out);
    EXPECT_EQ(lua_gettop(L), 0);  // consumed
    return out.Take();
  }
};

TEST(LuaNumberToInteger, TruncatesLikeTheCCast) {
  EXPECT_EQ(LuaNumberToInteger(100.5), 100);
  EXPECT_EQ(LuaNumberToInteger(-1.9), -1);
  EXPECT_EQ(LuaNumberToInteger(1e20), std::numeric_limits<int64_t>::min());
  EXPECT_EQ(LuaNumberToInteger(std::nan("")), std::numeric_limits<int64_t>::min());
  EXPECT_EQ(LuaNumberToInteger(-std::numeric_limits<double>::infinity()),
            std::numeric_limits<int64_t>::min());
  EXPECT_EQ(LuaNumberToInteger(-9223372036854775808.0), std::numeric_limits<int64_t>::min());
}

TEST(WriteLuaValue, ScalarsMatchRedis) {  // redis72-lua.md §4, tst:76-105
  Bare b;
  EXPECT_EQ(b.Reply("100.5"), ":100\r\n");
  EXPECT_EQ(b.Reply("'x'"), "$1\r\nx\r\n");
  EXPECT_EQ(b.Reply("true"), ":1\r\n");
  EXPECT_EQ(b.Reply("false"), "$-1\r\n");
  EXPECT_EQ(b.Reply("false", 2, 3), "_\r\n");
  EXPECT_EQ(b.Reply("nil"), "$-1\r\n");
  EXPECT_EQ(b.Reply("print or type"), "$-1\r\n");  // a function
  EXPECT_EQ(b.Reply("true", 3, 2), ":1\r\n");      // setresp(3): addReplyBool
  EXPECT_EQ(b.Reply("false", 3, 2), ":0\r\n");
  EXPECT_EQ(b.Reply("true", 3, 3), "#t\r\n");
  EXPECT_EQ(b.Reply("false", 3, 3), "#f\r\n");
}

TEST(WriteLuaValue, TablesMatchRedis) {
  Bare b;
  EXPECT_EQ(b.Reply("{1,2,3,'ciao',{1,2}}"), "*5\r\n:1\r\n:2\r\n:3\r\n$4\r\nciao\r\n*2\r\n:1\r\n:2\r\n");
  EXPECT_EQ(b.Reply("{1,nil,3}"), "*1\r\n:1\r\n");                 // nil truncates
  EXPECT_EQ(b.Reply("{1,false,3}"), "*3\r\n:1\r\n$-1\r\n:3\r\n");  // false does not
  EXPECT_EQ(b.Reply("{err='ERR this is an error'}"), "-ERR this is an error\r\n");
  EXPECT_EQ(b.Reply("{err='oops'}"), "-oops\r\n");  // no code added
  EXPECT_EQ(b.Reply("{ok='fine'}"), "+fine\r\n");
  EXPECT_EQ(b.Reply("{err='oops\\r\\n'}"), "-oops\r\n");
  EXPECT_EQ(b.Reply("{err='a\\r\\nb'}"), "-a  b\r\n");
  EXPECT_EQ(b.Reply("{ok='a\\0b'}"), "+a\r\n");
  EXPECT_EQ(b.Reply("{err='E x\\0y'}"), "-E x\r\n");
  EXPECT_EQ(b.Reply("{ok='a\\r\\nb'}"), "+a  b\r\n");
  EXPECT_EQ(b.Reply("{double=3.141}"), "$5\r\n3.141\r\n");
  EXPECT_EQ(b.Reply("{double=3.141}", 2, 3), ",3.141\r\n");
  EXPECT_EQ(b.Reply("{big_number='123\\r\\n123'}"), "$8\r\n123  123\r\n");
  EXPECT_EQ(b.Reply("{big_number='123'}", 2, 3), "(123\r\n");
  EXPECT_EQ(b.Reply("{verbatim_string={format='txt', string='hi'}}", 2, 3), "=6\r\ntxt:hi\r\n");
  EXPECT_EQ(b.Reply("{verbatim_string={format='txt', string='hi'}}"), "$2\r\nhi\r\n");
  EXPECT_EQ(b.Reply("{map={a=1}}"), "*2\r\n$1\r\na\r\n:1\r\n");
  EXPECT_EQ(b.Reply("{map={a=1}}", 2, 3), "%1\r\n$1\r\na\r\n:1\r\n");
  EXPECT_EQ(b.Reply("{set={a=true}}", 2, 3), "~1\r\n$1\r\na\r\n");
  EXPECT_EQ(b.Reply("_G and {} or 1"), "*0\r\n");
  // rawget: metatables are ignored (tst:66-74)
  EXPECT_EQ(b.Reply("setmetatable({}, {__index=function() error('boom') end})"), "*0\r\n");
}

TEST(WriteLuaValue, RecursiveTableEndsWithTheStackLimitError) {  // tst:853-867
  Bare b;
  const std::string r = b.Reply("(function() local a = {} a[1] = a return a end)()");
  EXPECT_EQ(r.substr(0, 4), "*1\r\n");
  const std::string tail = "-ERR reached lua stack limit\r\n";
  ASSERT_GE(r.size(), tail.size());
  EXPECT_EQ(r.substr(r.size() - tail.size()), tail);
  EXPECT_EQ(lua_gettop(b.L), 0);
}

std::string ErrField(lua_State* L) {
  lua_getfield(L, -1, "err");
  std::string s = lua_tostring(L, -1);
  lua_pop(L, 2);
  return s;
}

TEST(PushErrorTable, NormalisesLikeRedis) {  // sl:531-575
  Bare b;
  PushErrorTable(b.L, "-WRONGTYPE Operation against a key");
  EXPECT_EQ(ErrField(b.L), "WRONGTYPE Operation against a key");
  PushErrorTable(b.L, "Unknown Redis command called from script");
  EXPECT_EQ(ErrField(b.L), "ERR Unknown Redis command called from script");
  PushErrorTable(b.L, "-oops");
  EXPECT_EQ(ErrField(b.L), "ERR oops");
  PushErrorTable(b.L, "-");
  EXPECT_EQ(ErrField(b.L), "ERR ");
  PushErrorTable(b.L, "-ERR x\r\n");
  EXPECT_EQ(ErrField(b.L), "ERR x");
}

// Pushes `reply` as a Lua value into global r, then evaluates `check`.
bool Check(Bare& b, std::string_view reply, const std::string& check) {
  auto node = resp::ParseReply(reply);
  EXPECT_TRUE(node.ok());
  EXPECT_TRUE(PushReply(b.L, *node));
  lua_setglobal(b.L, "r");
  b.Push(check);
  const bool ok = lua_toboolean(b.L, -1) != 0;
  lua_pop(b.L, 1);
  return ok;
}

TEST(PushReply, MatchesRedis) {  // sl:213-523
  Bare b;
  EXPECT_TRUE(Check(b, ":5\r\n", "r == 5"));
  EXPECT_TRUE(Check(b, "$1\r\nx\r\n", "r == 'x'"));
  EXPECT_TRUE(Check(b, "$-1\r\n", "r == false"));
  EXPECT_TRUE(Check(b, "*-1\r\n", "r == false"));
  EXPECT_TRUE(Check(b, "_\r\n", "r == nil"));
  EXPECT_TRUE(Check(b, "+OK\r\n", "r.ok == 'OK'"));
  EXPECT_TRUE(
      Check(b, "-WRONGTYPE bad\r\n", "r.err == 'WRONGTYPE bad' and r.ignore_error_stats_update == true"));
  EXPECT_TRUE(Check(b, "*2\r\n:1\r\n$1\r\na\r\n", "r[1] == 1 and r[2] == 'a' and #r == 2"));
  EXPECT_TRUE(Check(b, "#t\r\n", "r == true"));
  EXPECT_TRUE(Check(b, ",1.5\r\n", "r.double == 1.5"));
  EXPECT_TRUE(Check(b, "%1\r\n$1\r\nk\r\n:1\r\n", "r.map.k == 1"));
  EXPECT_TRUE(Check(b, "~1\r\n$1\r\na\r\n", "r.set.a == true"));
  EXPECT_TRUE(Check(b, "(123\r\n", "r.big_number == '123'"));
  EXPECT_TRUE(
      Check(b, "=6\r\ntxt:hi\r\n", "r.verbatim_string.format == 'txt' and r.verbatim_string.string == 'hi'"));
}

// Captures redis.call-style arguments.
std::optional<std::vector<std::string>> g_argv;
std::string g_error;
int Capture(lua_State* L) {
  g_argv = ArgsToArgv(L, &g_error);
  return 0;
}

TEST(ArgsToArgv, ConvertsLikeRedis) {  // sl:792-865
  Bare b;
  lua_register(b.L, "capture", Capture);
  b.Push("capture('set', 1, 2.5, 9007199254740991, 1e20, -0) or true");
  lua_pop(b.L, 1);
  ASSERT_TRUE(g_argv.has_value());
  EXPECT_EQ(*g_argv, (std::vector<std::string>{"set", "1", "2.5", "9007199254740991", "1e+20", "0"}));
  b.Push("capture() or true");
  lua_pop(b.L, 1);
  EXPECT_FALSE(g_argv.has_value());
  EXPECT_EQ(g_error, "Please specify at least one argument for this redis lib call");
  b.Push("capture('get', {}) or true");
  lua_pop(b.L, 1);
  EXPECT_FALSE(g_argv.has_value());
  EXPECT_EQ(g_error, "Lua redis lib command arguments must be strings or integers");
}

}  // namespace
}  // namespace tideskv
