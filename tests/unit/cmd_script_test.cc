#include <gtest/gtest.h>

#include "script/sha1hex.h"
#include "support/command_harness.h"

namespace tideskv::test {
namespace {

std::string Suffix(const std::string& code, int line = 1) {
  return " script: " + Sha1Hex(code) + ", on @user_script:" + std::to_string(line) + ".";
}

TEST(EvalCmd, BasicsAndKeys) {
  CommandHarness h;
  EXPECT_EQ(h.Run({"EVAL", "return redis.call('set', KEYS[1], ARGV[1])", "1", "k", "v"}), kOk);
  EXPECT_EQ(h.Run({"GET", "k"}), Bulk("v"));
  EXPECT_EQ(h.Run({"EVAL", "return {KEYS[1], ARGV[1], ARGV[2]}", "1", "a", "b", "c"}),
            Arr({Bulk("a"), Bulk("b"), Bulk("c")}));
  EXPECT_EQ(h.Run({"EVAL", "", "0"}), kNil);
}

TEST(EvalCmd, ArgumentErrorsInRedisOrder) {  // redis72-lua.md §1
  CommandHarness h;
  EXPECT_EQ(h.Run({"EVAL", "return 1"}), Err("ERR wrong number of arguments for 'eval' command"));
  EXPECT_EQ(h.Run({"EVAL", "return 1", "x"}), Err("ERR value is not an integer or out of range"));
  EXPECT_EQ(h.Run({"EVAL", "return 1", "2", "a"}),
            Err("ERR Number of keys can't be greater than number of args"));
  EXPECT_EQ(h.Run({"EVAL", "return 1", "-1"}), Err("ERR Number of keys can't be negative"));
  EXPECT_EQ(h.Run({"EVALSHA", "short", "x"}), Err("NOSCRIPT No matching script. Please use EVAL."));
  EXPECT_EQ(h.Run({"EVALSHA", std::string(40, 'f'), "x"}),
            Err("ERR value is not an integer or out of range"));
  EXPECT_EQ(h.Run({"EVALSHA", std::string(40, 'f'), "0"}),
            Err("NOSCRIPT No matching script. Please use EVAL."));
}

TEST(EvalCmd, ErrorsCarryTheScriptSuffix) {
  CommandHarness h;
  h.Run({"SET", "s", "x"});
  const std::string c = "return redis.call('lpush', KEYS[1], 'x')";
  EXPECT_EQ(h.Run({"EVAL", c, "1", "s"}),
            Err("WRONGTYPE Operation against a key holding the wrong kind of value" + Suffix(c)));
  const std::string compile = h.Run({"EVAL", "return =", "0"});
  EXPECT_EQ(compile.rfind("-ERR Error compiling script (new function): user_script:1:", 0), 0u) << compile;
}

TEST(EvalCmd, NoRollbackOnRuntimeError) {  // tst:1208-1214
  CommandHarness h;
  const std::string c = "redis.call('incr', KEYS[1]) redis.call('nosuch')";
  EXPECT_EQ(h.Run({"EVAL", c, "1", "x"}), Err("ERR Unknown Redis command called from script" + Suffix(c)));
  EXPECT_EQ(h.Run({"GET", "x"}), Bulk("1"));
}

TEST(EvalCmd, ReadOnlyVariants) {
  CommandHarness h;
  const std::string w = "return redis.call('set', KEYS[1], 'v')";
  EXPECT_EQ(h.Run({"EVAL_RO", w, "1", "k"}),
            Err("ERR Write commands are not allowed from read-only scripts." + Suffix(w)));
  EXPECT_EQ(h.Run({"EVAL_RO", "return redis.call('get', KEYS[1])", "1", "k"}), kNil);
  EXPECT_EQ(h.Run({"EVAL_RO", "#!lua\nreturn 1", "0"}),
            Err("ERR Can not execute a script with write flag using *_ro command."));
  const std::string nw = "#!lua flags=no-writes\nreturn redis.call('set', KEYS[1], 'v')";
  EXPECT_EQ(h.Run({"EVAL", nw, "1", "k"}),
            Err("ERR Write commands are not allowed from read-only scripts." + Suffix(nw, 2)));
  const std::string sha = Sha1Hex("return redis.call('get', KEYS[1])");
  EXPECT_EQ(h.Run({"EVALSHA_RO", sha, "1", "k"}), kNil);  // cached by the EVAL_RO above
}

TEST(EvalCmd, ScriptSelectDoesNotLeakAndBlockingAnswersAtOnce) {
  CommandHarness h;
  EXPECT_EQ(h.Run({"EVAL", "redis.call('select', 5) return redis.call('set','k','v')", "0"}), kOk);
  EXPECT_EQ(h.session.db, 0);
  EXPECT_EQ(h.Run({"GET", "k"}), kNil);
  EXPECT_EQ(h.Run({"EVAL", "return redis.call('blpop', 'q', 0)", "0"}), kNil);
}

TEST(EvalCmd, WaiterIsServedAfterTheScript) {
  CommandHarness h;
  SecondClient w(h);
  EXPECT_EQ(w.Run({"BLPOP", "q", "0"}), "");
  EXPECT_EQ(h.Run({"EVAL", "redis.call('lpush','q','x') return redis.call('llen','q')", "0"}), Int(1));
  EXPECT_EQ(w.TakeUnblocked(), (std::vector<std::string>{Arr({Bulk("q"), Bulk("x")})}));
}

TEST(EvalCmd, InsideMultiAndWatch) {
  CommandHarness h;
  SecondClient other(h);
  h.Run({"MULTI"});
  EXPECT_EQ(h.Run({"EVAL", "return redis.call('incr','n')", "0"}), "+QUEUED\r\n");
  h.Run({"INCR", "n"});
  EXPECT_EQ(h.Run({"EXEC"}), Arr({Int(1), Int(2)}));
  h.Run({"WATCH", "w"});
  other.Run({"EVAL", "return redis.call('set','w','1')", "0"});
  h.Run({"MULTI"});
  EXPECT_EQ(h.Run({"EXEC"}), "*-1\r\n");
}

// Redis refuses an unknown or mis-sized SCRIPT subcommand at lookup time, so inside MULTI it is not
// queued and it dooms the transaction (differential seed 59, Redis 7.2.5: `SCRIPT FOO` in MULTI
// answers -ERR unknown subcommand 'FOO'. Try SCRIPT HELP.).
TEST(ScriptCmd, BadSubcommandInsideMultiIsRejectedAtQueueTime) {
  CommandHarness h;
  h.Run({"MULTI"});
  EXPECT_EQ(h.Run({"SCRIPT", "FOO"}), Err("ERR unknown subcommand 'FOO'. Try SCRIPT HELP."));
  EXPECT_EQ(h.Run({"SCRIPT", "LOAD"}), Err("ERR wrong number of arguments for 'script|load' command"));
  EXPECT_EQ(h.Run({"SCRIPT", "FLUSH"}), "+QUEUED\r\n");
  EXPECT_EQ(h.Run({"EXEC"}), Err("EXECABORT Transaction discarded because of previous errors."));
}

TEST(ScriptCmd, UnknownSubcommandEchoIsTruncatedTo128Bytes) {  // Redis: "unknown subcommand '%.128s'"
  CommandHarness h;
  const std::string name(200, 'x');
  EXPECT_EQ(h.Run({"SCRIPT", name}),
            Err("ERR unknown subcommand '" + std::string(128, 'x') + "'. Try SCRIPT HELP."));
  h.Run({"MULTI"});  // the queue-time check builds the same text
  EXPECT_EQ(h.Run({"SCRIPT", name}),
            Err("ERR unknown subcommand '" + std::string(128, 'x') + "'. Try SCRIPT HELP."));
}

TEST(EvalCmd, FailedCommitRepliesTheErrorAndAppliesNothing) {
  CommandHarness h;
  h.services.storage_.fail_commit_in = 1;
  EXPECT_EQ(h.Run({"EVAL", "return redis.call('set','k','v')", "0"}).substr(0, 5), "-ERR ");
  EXPECT_EQ(h.Run({"GET", "k"}), kNil);
}

TEST(EvalCmd, EvalIsNotAllowedFromScript) {  // controller ruling R1
  CommandHarness h;
  const std::string c = "return redis.call('eval', 'return 1', '0')";
  EXPECT_EQ(h.Run({"EVAL", c, "0"}), Err("ERR This Redis command is not allowed from script" + Suffix(c)));
}

TEST(ScriptCmd, LoadExistsFlushHelpDebugKill) {
  CommandHarness h;
  const std::string sha = Sha1Hex("return 'loaded'");
  EXPECT_EQ(h.Run({"SCRIPT", "LOAD", "return 'loaded'"}), Bulk(sha));
  EXPECT_EQ(h.Run({"EVALSHA", sha, "0"}), Bulk("loaded"));
  EXPECT_EQ(h.Run({"SCRIPT", "EXISTS", sha, "nope"}), Arr({Int(1), Int(0)}));
  EXPECT_EQ(h.Run({"SCRIPT", "FLUSH"}), kOk);
  EXPECT_EQ(h.Run({"SCRIPT", "EXISTS", sha}), Arr({Int(0)}));
  EXPECT_EQ(h.Run({"SCRIPT", "FLUSH", "ASYNC"}), kOk);
  EXPECT_EQ(h.Run({"SCRIPT", "FLUSH", "sync"}), kOk);
  EXPECT_EQ(h.Run({"SCRIPT", "FLUSH", "x"}), Err("ERR SCRIPT FLUSH only support SYNC|ASYNC option"));
  EXPECT_EQ(h.Run({"SCRIPT", "KILL"}), Err("NOTBUSY No scripts in execution right now."));
  EXPECT_EQ(h.Run({"SCRIPT", "DEBUG", "NO"}), kOk);
  EXPECT_EQ(h.Run({"SCRIPT", "DEBUG", "YES"}), Err("ERR SCRIPT DEBUG YES/SYNC is not supported"));
  EXPECT_EQ(h.Run({"SCRIPT", "DEBUG", "x"}), Err("ERR Use SCRIPT DEBUG YES/SYNC/NO"));
  EXPECT_EQ(h.Run({"SCRIPT", "FOO"}), Err("ERR unknown subcommand 'FOO'. Try SCRIPT HELP."));
  EXPECT_EQ(h.Run({"SCRIPT", "LOAD"}), Err("ERR wrong number of arguments for 'script|load' command"));
  EXPECT_EQ(h.Run({"SCRIPT", "LOAD", "#!lua flags=bad\nreturn 1"}),
            Err("ERR Unexpected flag in script shebang: bad"));
  // redis72-lua.md §1 (ev:580-598): 1 header, 14 body lines, 2 footer lines, all status replies.
  const std::string expected_help =
      "*17\r\n"
      "+SCRIPT <subcommand> [<arg> [value] [opt] ...]. Subcommands are:\r\n"
      "+DEBUG (YES|SYNC|NO)\r\n"
      "+    Set the debug mode for subsequent scripts executed.\r\n"
      "+EXISTS <sha1> [<sha1> ...]\r\n"
      "+    Return information about the existence of the scripts in the script cache.\r\n"
      "+FLUSH [ASYNC|SYNC]\r\n"
      "+    Flush the Lua scripts cache. Very dangerous on replicas.\r\n"
      "+    When called without the optional mode argument, the behavior is determined by the\r\n"
      "+    lazyfree-lazy-user-flush configuration directive. Valid modes are:\r\n"
      "+    * ASYNC: Asynchronously flush the scripts cache.\r\n"
      "+    * SYNC: Synchronously flush the scripts cache.\r\n"
      "+KILL\r\n"
      "+    Kill the currently executing Lua script.\r\n"
      "+LOAD <script>\r\n"
      "+    Load a script into the scripts cache without executing it.\r\n"
      "+HELP\r\n"
      "+    Print this help.\r\n";
  EXPECT_EQ(h.Run({"SCRIPT", "HELP"}), expected_help);
}

TEST(ScriptCmd, InfoMemoryAndConfig) {
  CommandHarness h;
  h.Run({"SCRIPT", "LOAD", "return 1"});
  EXPECT_EQ(h.Run({"CONFIG", "GET", "lua-time-limit"}), Arr({Bulk("lua-time-limit"), Bulk("5000")}));
  EXPECT_EQ(h.Run({"CONFIG", "SET", "busy-reply-threshold", "100"}), kOk);
  EXPECT_EQ(h.Run({"CONFIG", "GET", "lua-time-limit"}), Arr({Bulk("lua-time-limit"), Bulk("100")}));
}

}  // namespace
}  // namespace tideskv::test
