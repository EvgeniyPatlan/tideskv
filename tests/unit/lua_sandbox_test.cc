#include "script/lua_sandbox.h"

#include <gtest/gtest.h>

#include <map>

#include "script/rand48.h"
#include "script/sha1hex.h"

namespace tideskv {
namespace {

class FakeHost final : public SandboxHost {
 public:
  std::optional<std::string> Call(const std::vector<std::string>& argv, int resp,
                                  std::string* error) noexcept override {
    calls.push_back(argv);
    last_resp = resp;
    if (auto it = rejections.find(argv[0]); it != rejections.end()) {
      *error = it->second;
      return std::nullopt;
    }
    auto it = replies.find(argv[0]);
    return it == replies.end() ? std::string("+OK\r\n") : it->second;
  }
  bool CommandExists(std::string_view name) const noexcept override { return name == "get" || name == "set"; }
  void Log(int level, std::string_view msg) noexcept override { logs.emplace_back(level, std::string(msg)); }
  bool ShouldKill() noexcept override { return ++hooks >= kill_after; }

  std::map<std::string, std::string> replies;     // argv[0] -> reply bytes
  std::map<std::string, std::string> rejections;  // argv[0] -> rejection text
  std::vector<std::vector<std::string>> calls;
  std::vector<std::pair<int, std::string>> logs;
  int last_resp = 0;
  int hooks = 0;
  int kill_after = 1 << 30;
};

struct SandboxTest : ::testing::Test {
  std::unique_ptr<LuaSandbox> sb = LuaSandbox::Create();
  FakeHost host;
  bool killed = false;

  std::string Eval(const std::string& code, std::vector<std::string> keys = {},
                   std::vector<std::string> args = {}, int caller_resp = 2) {
    const std::string sha = Sha1Hex(code);
    const Status st = sb->Compile(sha, code);
    if (!st.ok()) return "COMPILE:" + st.message();
    resp::ReplyBuilder out(caller_resp);
    killed = sb->Run(sha, keys, args, host, {caller_resp, true}, out).killed;
    return out.Take();
  }
  // "-<msg> script: <sha>, on @user_script:<line>.\r\n"
  static std::string ScriptErr(const std::string& code, const std::string& msg, int line = 1) {
    return "-" + msg + " script: " + Sha1Hex(code) + ", on @user_script:" + std::to_string(line) + ".\r\n";
  }
};

TEST_F(SandboxTest, RunsWithKeysAndArgv) {
  EXPECT_EQ(Eval("return 1"), ":1\r\n");
  EXPECT_EQ(Eval("return KEYS[1] .. ARGV[1] .. #ARGV", {"k"}, {"a", "b"}), "$3\r\nka2\r\n");
  EXPECT_EQ(Eval("return KEYS[1]"), "$-1\r\n");  // fresh KEYS every run
  EXPECT_GT(sb->memory_bytes(), 0u);
}

TEST_F(SandboxTest, CompileErrors) {
  EXPECT_EQ(Eval("return ="),
            "COMPILE:ERR Error compiling script (new function): user_script:1: "
            "unexpected symbol near '='");
}

TEST_F(SandboxTest, ErrorRepliesMatchRedis) {  // redis72-lua.md §5
  std::string c = "x = 1";
  EXPECT_EQ(Eval(c), ScriptErr(c, "ERR user_script:1: Attempt to modify a readonly table"));
  c = "return a";
  EXPECT_EQ(Eval(c),
            ScriptErr(c, "ERR user_script:1: Script attempted to access nonexistent global variable 'a'"));
  c = "error('x')";
  EXPECT_EQ(Eval(c), ScriptErr(c, "ERR user_script:1: x"));
  c = "error('x', 0)";
  EXPECT_EQ(Eval(c), ScriptErr(c, "ERR x"));
  c = "error({err='MY x'})";
  EXPECT_EQ(Eval(c), ScriptErr(c, "MY x"));
  c = "\nreturn 1 + nil";
  EXPECT_EQ(Eval(c), ScriptErr(c, "ERR user_script:2: attempt to perform arithmetic on a nil value", 2));
}

TEST_F(SandboxTest, RedisCallErrorsCarryTheSuffixEvenInATailCall) {
  host.replies["lpush"] = "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";
  std::string c = "return redis.call('lpush','str','x')";
  EXPECT_EQ(Eval(c), ScriptErr(c, "WRONGTYPE Operation against a key holding the wrong kind of value"));
  c = "return redis.pcall('lpush','str','x')";
  EXPECT_EQ(Eval(c), "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n");
  host.rejections["nosuch"] = "Unknown Redis command called from script";
  c = "return redis.call('nosuch')";
  EXPECT_EQ(Eval(c), ScriptErr(c, "ERR Unknown Redis command called from script"));
  c = "redis.call()";
  EXPECT_EQ(Eval(c), ScriptErr(c, "ERR Please specify at least one argument for this redis lib call"));
  EXPECT_EQ(Eval("return redis.pcall()"),
            "-ERR Please specify at least one argument for this redis lib call\r\n");
  EXPECT_EQ(Eval("local ok, e = pcall(redis.call, 'nosuch') return e"),
            "$44\r\nERR Unknown Redis command called from script\r\n");
  EXPECT_EQ(Eval("return {pcall(error, {err='custom'})}"), "*2\r\n$-1\r\n$6\r\ncustom\r\n");
}

TEST_F(SandboxTest, RedisCallConvertsArgumentsAndReplies) {
  host.replies["get"] = "$3\r\nbar\r\n";
  EXPECT_EQ(Eval("return redis.call('get', KEYS[1], 5)", {"foo"}), "$3\r\nbar\r\n");
  EXPECT_EQ(host.calls.back(), (std::vector<std::string>{"get", "foo", "5"}));
  EXPECT_EQ(host.last_resp, 2);
  Eval("redis.setresp(3) return redis.call('get','x')");
  EXPECT_EQ(host.last_resp, 3);
  Eval("return redis.call('get','x')");
  EXPECT_EQ(host.last_resp, 2);  // back to 2 on every run (script.c:218)
}

TEST_F(SandboxTest, SandboxAbsencesAndPresences) {  // redis72-lua.md §3
  for (const char* g : {"print", "loadfile", "dofile", "os", "io", "debug", "require", "module", "getfenv",
                        "setfenv", "newproxy", "jit"}) {
    const std::string c = std::string("return ") + g;
    EXPECT_EQ(
        Eval(c),
        ScriptErr(c,
                  std::string("ERR user_script:1: Script attempted to access nonexistent global variable '") +
                      g + "'"))
        << g;
  }
  EXPECT_EQ(
      Eval("return {type(loadstring), type(load), type(coroutine), type(gcinfo), type(unpack), "
           "type(xpcall), type(cjson), type(cmsgpack), type(bit), type(struct), _VERSION}"),
      "*11\r\n$8\r\nfunction\r\n$8\r\nfunction\r\n$5\r\ntable\r\n$8\r\nfunction\r\n$8\r\nfunction\r\n"
      "$8\r\nfunction\r\n$5\r\ntable\r\n$5\r\ntable\r\n$5\r\ntable\r\n$5\r\ntable\r\n$7\r\nLua 5.1\r\n");
}

TEST_F(SandboxTest, ProtectionCoversEverythingReachable) {
  for (const char* c :
       {"redis.call = nil", "rawset(_G, 'x', 1)", "setmetatable(_G, {})", "_G.x = 1", "cjson.encode = nil",
        "getmetatable('').__index = {}", "string.len = nil", "getmetatable(_G).__index = {}"}) {
    const std::string r = Eval(c);
    EXPECT_NE(r.find("Attempt to modify a readonly table"), std::string::npos) << c << " -> " << r;
  }
  EXPECT_EQ(Eval("return loadstring(string.dump(function() return 1 end))"), "$-1\r\n");
}

TEST_F(SandboxTest, CjsonSettingsPersistLikeRedis) {
  EXPECT_EQ(Eval("cjson.encode_max_depth(5) return 1"), ":1\r\n");
  EXPECT_EQ(Eval("return cjson.encode_max_depth()"), ":5\r\n");
}

TEST_F(SandboxTest, MathRandomIsRedisRand48) {  // lua-integration.md §3.4
  RedisRand48ResetForTests();
  EXPECT_EQ(Eval("return {math.random(1000), math.random(1000)}"), "*2\r\n:397\r\n:841\r\n");
  EXPECT_EQ(Eval("math.randomseed(1) return math.random(1000)"), ":42\r\n");
  // Live redis:7.2.5 replies for luaL_error / luaL_argcheck / luaL_checkint raisers (all carry luaL_where).
  std::string c = "return math.random(1, 2, 3)";
  EXPECT_EQ(Eval(c), ScriptErr(c, "ERR user_script:1: wrong number of arguments"));
  c = "return math.random(0)";
  EXPECT_EQ(Eval(c), ScriptErr(c, "ERR user_script:1: bad argument #1 to 'random' (interval is empty)"));
  c = "return math.random(2, 1)";
  EXPECT_EQ(Eval(c), ScriptErr(c, "ERR user_script:1: bad argument #2 to 'random' (interval is empty)"));
  c = "math.randomseed('x')";
  EXPECT_EQ(Eval(c),
            ScriptErr(c, "ERR user_script:1: bad argument #1 to 'randomseed' (number expected, got string)"));
}

TEST_F(SandboxTest, RedisHelpers) {  // redis72-lua.md §3 table
  EXPECT_EQ(Eval("return redis.sha1hex('')"), "$40\r\nda39a3ee5e6b4b0d3255bfef95601890afd80709\r\n");
  std::string c = "return redis.sha1hex()";
  EXPECT_EQ(Eval(c), ScriptErr(c, "ERR wrong number of arguments"));
  EXPECT_EQ(Eval("return redis.error_reply('MY_ERR custom msg')"), "-MY_ERR custom msg\r\n");
  EXPECT_EQ(Eval("return redis.error_reply('oops')"), "-ERR oops\r\n");
  EXPECT_EQ(Eval("return redis.error_reply('')"), "-ERR \r\n");
  EXPECT_EQ(Eval("return redis.error_reply(1, 2)"), "-ERR wrong number or type of arguments\r\n");
  EXPECT_EQ(Eval("return redis.status_reply('FINE')"), "+FINE\r\n");
  c = "redis.setresp(4)";
  EXPECT_EQ(Eval(c), ScriptErr(c, "ERR RESP version must be 2 or 3."));
  c = "redis.setresp()";
  EXPECT_EQ(Eval(c), ScriptErr(c, "ERR redis.setresp() requires one argument."));
  EXPECT_EQ(Eval("redis.setresp(3) return true"), ":1\r\n");
  EXPECT_EQ(Eval("redis.setresp(3) return true", {}, {}, 3), "#t\r\n");
  EXPECT_EQ(Eval("return redis.replicate_commands()"), ":1\r\n");
  EXPECT_EQ(Eval("return redis.set_repl(redis.REPL_ALL)"), "$-1\r\n");
  c = "redis.set_repl(12345)";
  EXPECT_EQ(
      Eval(c),
      ScriptErr(c, "ERR Invalid replication flags. Use REPL_AOF, REPL_REPLICA, REPL_ALL or REPL_NONE."));
  EXPECT_EQ(Eval("return redis.breakpoint()"), "$-1\r\n");
  EXPECT_EQ(Eval("return redis.REDIS_VERSION"), "$5\r\n7.2.0\r\n");
  EXPECT_EQ(Eval("return redis.REDIS_VERSION_NUM"), ":459264\r\n");  // 0x00070200
  EXPECT_EQ(Eval("return redis.acl_check_cmd('get')"), ":1\r\n");
  c = "return redis.acl_check_cmd('nosuch')";
  EXPECT_EQ(Eval(c), ScriptErr(c, "ERR Invalid command passed to redis.acl_check_cmd()"));
  EXPECT_EQ(Eval("redis.log(redis.LOG_WARNING, 'a', 1, 'b') return 1"), ":1\r\n");
  ASSERT_FALSE(host.logs.empty());
  EXPECT_EQ(host.logs.back(), (std::pair<int, std::string>{3, "a 1 b"}));
  c = "redis.log(1)";
  EXPECT_EQ(Eval(c), ScriptErr(c, "ERR redis.log() requires two arguments or more."));
  c = "redis.log(9, 'x')";
  EXPECT_EQ(Eval(c), ScriptErr(c, "ERR Invalid debug level."));
}

TEST_F(SandboxTest, HookKillsAndPcallCannotSwallowIt) {
  host.kill_after = 3;
  std::string c = "while true do end";
  EXPECT_EQ(Eval(c), ScriptErr(c, "ERR Script killed by user with SCRIPT KILL..."));
  EXPECT_TRUE(killed);
  host.hooks = 0;
  c = "while true do pcall(function() while true do end end) end";
  const std::string r = Eval(c);
  EXPECT_NE(r.find("Script killed by user with SCRIPT KILL..."), std::string::npos) << r;
  EXPECT_TRUE(killed);
  host.kill_after = 1 << 30;
  EXPECT_EQ(Eval("return 1"), ":1\r\n");  // the next script runs normally
}

TEST_F(SandboxTest, ArgumentErrorsFreeTheirArgvBeforeRaising) {  // checked for leaks under ASan
  std::string c = "return redis.call('get', 'a', {})";
  EXPECT_EQ(Eval(c), ScriptErr(c, "ERR Lua redis lib command arguments must be strings or integers"));
  EXPECT_EQ(Eval("return redis.pcall('get', 'a', {})"),
            "-ERR Lua redis lib command arguments must be strings or integers\r\n");
  c = "return redis.acl_check_cmd('get', {})";
  EXPECT_EQ(Eval(c), ScriptErr(c, "ERR Lua redis lib command arguments must be strings or integers"));
  EXPECT_EQ(Eval("for i = 1, 100 do pcall(redis.call, 'nosuch', i) end return 1"), ":1\r\n");
}

TEST_F(SandboxTest, AnErrorTableWhoseFieldsRaiseDoesNotCrash) {
  const std::string c = "error(setmetatable({}, {__index = function() error('boom', 0) end}))";
  EXPECT_EQ(Eval(c), "-ERR Error running script " + Sha1Hex(c) + ", boom\r\n");
  EXPECT_EQ(Eval("return 1"), ":1\r\n");
}

// A host that asks for the kill only once: the kill must still be sticky for the run.
class OneShotKillHost final : public SandboxHost {
 public:
  std::optional<std::string> Call(const std::vector<std::string>& /*argv*/, int /*resp*/,
                                  std::string* /*error*/) noexcept override {
    return std::string("+OK\r\n");
  }
  bool CommandExists(std::string_view /*name*/) const noexcept override { return false; }
  void Log(int /*level*/, std::string_view /*msg*/) noexcept override {}
  bool ShouldKill() noexcept override { return ++polls == 1; }
  int polls = 0;
};

TEST_F(SandboxTest, KillIsStickyWhenTheHostAsksOnlyOnce) {
  OneShotKillHost once;
  const std::string c = "while true do pcall(function() while true do end end) end";
  const std::string sha = Sha1Hex(c);
  ASSERT_TRUE(sb->Compile(sha, c).ok());
  resp::ReplyBuilder out(2);
  EXPECT_TRUE(sb->Run(sha, {}, {}, once, {2, true}, out).killed);
  EXPECT_NE(out.Take().find("Script killed by user with SCRIPT KILL..."), std::string::npos);
  EXPECT_EQ(once.polls, 1);
}

// Regression guard against the NULL dereference Redis 7.2.16 has here, not a parity claim.
TEST_F(SandboxTest, AnErrorTableWithoutErrRepliesWithTheSuffixOnly) {
  const std::string c = "error({})";
  EXPECT_EQ(Eval(c), "- script: " + Sha1Hex(c) + ", on @user_script:1.\r\n");
}

TEST_F(SandboxTest, MathRandomFullIntRangeHasNoOverflow) {
  const std::string r = Eval("return math.random(-2147483647, 2147483647)");
  ASSERT_FALSE(r.empty());
  EXPECT_EQ(r.front(), ':') << r;
}

}  // namespace
}  // namespace tideskv
