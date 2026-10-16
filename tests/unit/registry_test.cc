#include "commands/registry.h"

#include <gtest/gtest.h>

#include <set>
#include <stdexcept>

#include "support/command_harness.h"
#include "support/fake_server_services.h"

namespace tideskv {
namespace {

void EchoHandler(ExecContext& ctx, const ArgList& argv) { ctx.reply.Bulk(argv[1]); }
void CountHandler(ExecContext& ctx, const ArgList& argv) {
  ctx.reply.Integer(static_cast<int64_t>(argv.size()));
}

class RegistryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    services_.registry_.Register({"echo", 2, kCmdFast, 0, 0, 0, &EchoHandler});
    services_.registry_.Register({"count", -2, 0, 1, -1, 1, &CountHandler});
    services_.registry_.Register({"open", -1, kCmdNoAuth, 0, 0, 0, &CountHandler});
    session_.authenticated = true;
  }
  std::string Run(const ArgList& argv) {
    ExecContext ctx{session_, reply_, services_};
    Dispatch(services_.registry(), ctx, argv);
    return reply_.Take();
  }

  test::FakeServerServices services_;
  Session session_;
  resp::ReplyBuilder reply_;
};

TEST_F(RegistryTest, FindIsCaseInsensitive) {
  ASSERT_NE(services_.registry_.Find("ECHO"), nullptr);
  EXPECT_EQ(services_.registry_.Find("EcHo")->name, "echo");
  EXPECT_EQ(services_.registry_.Find("nope"), nullptr);
  EXPECT_EQ(services_.registry_.size(), 3u);
}

TEST_F(RegistryTest, DuplicateRegistrationThrows) {
  EXPECT_THROW(services_.registry_.Register({"ECHO", 2, 0, 0, 0, 0, &EchoHandler}), std::logic_error);
}

TEST_F(RegistryTest, WriteCommandsAreRefusedOnAReplica) {
  services_.registry_.Register({"wr", 1, kCmdWrite, 0, 0, 0, &CountHandler});
  EXPECT_EQ(Run({"wr"}), ":1\r\n");
  services_.replication_.set_role(ReplRole::kReplica);
  EXPECT_EQ(Run({"wr"}), "-READONLY You can't write against a read only replica.\r\n");
  EXPECT_EQ(Run({"echo", "x"}), "$1\r\nx\r\n");
}

TEST_F(RegistryTest, RunsHandler) { EXPECT_EQ(Run({"ECHO", "hi"}), "$2\r\nhi\r\n"); }

TEST_F(RegistryTest, UnknownCommandListsArgs) {
  EXPECT_EQ(Run({"nope", "a", "b"}), "-ERR unknown command 'nope', with args beginning with: 'a' 'b' \r\n");
  EXPECT_EQ(Run({"nope"}), "-ERR unknown command 'nope', with args beginning with: \r\n");
}

// Redis 7.2 formats the name with "%.128s" and the args with "'%.*s' " while the args text is
// shorter than 128 bytes, so an embedded NUL ends a string and the args stop growing at ~128.
TEST_F(RegistryTest, UnknownCommandEchoesLikeRedisPrintf) {
  using namespace std::string_literals;
  EXPECT_EQ(Run({"nope", "bin\0x"s}), "-ERR unknown command 'nope', with args beginning with: 'bin' \r\n");
  EXPECT_EQ(Run({"no\0pe"s, "a"}), "-ERR unknown command 'no', with args beginning with: 'a' \r\n");
  const std::string a(100, 'a');
  // First arg takes 103 bytes; the second is cut to the 25 bytes left; the third is not echoed.
  EXPECT_EQ(Run({"nope", a, a, "c"}), "-ERR unknown command 'nope', with args beginning with: '" + a + "' '" +
                                          std::string(25, 'a') + "' \r\n");
}

// Redis 7.2.5 server.c:3778-3788: name "%.128s"; args loop runs while the args text is < 128 bytes and
// appends "'%.*s' " limited to 128 - len bytes. An argument of N bytes adds N + 3 bytes ("'" + N + "' ").
TEST_F(RegistryTest, UnknownCommandNameIsCutAt128Bytes) {
  EXPECT_EQ(Run({std::string(200, 'n')}),
            "-ERR unknown command '" + std::string(128, 'n') + "', with args beginning with: \r\n");
}

TEST_F(RegistryTest, UnknownCommandArgsBudgetBoundary) {
  const std::string head = "-ERR unknown command 'nope', with args beginning with: ";
  // 124-byte arg -> 124 + 3 = 127 bytes < 128, so the loop continues with room = 128 - 127 = 1:
  // the next arg "xyz" is appended cut to 1 byte, "'x' ".
  const std::string a124(124, 'a');
  EXPECT_EQ(Run({"nope", a124, "xyz", "tail"}), head + "'" + a124 + "' 'x' \r\n");
  // 125-byte arg -> 125 + 3 = 128 bytes, not < 128, so the loop stops: "xyz" is dropped.
  const std::string a125(125, 'a');
  EXPECT_EQ(Run({"nope", a125, "xyz"}), head + "'" + a125 + "' \r\n");
}

TEST_F(RegistryTest, ArityExactAndMinimum) {
  EXPECT_EQ(Run({"echo"}), "-ERR wrong number of arguments for 'echo' command\r\n");
  EXPECT_EQ(Run({"echo", "a", "b"}), "-ERR wrong number of arguments for 'echo' command\r\n");
  EXPECT_EQ(Run({"count"}), "-ERR wrong number of arguments for 'count' command\r\n");
  EXPECT_EQ(Run({"count", "a", "b", "c"}), ":4\r\n");
}

TEST_F(RegistryTest, NoAuthGate) {
  session_.authenticated = false;
  EXPECT_EQ(Run({"echo", "x"}), "-NOAUTH Authentication required.\r\n");
  EXPECT_EQ(Run({"open"}), ":1\r\n");
  EXPECT_EQ(Run({"nope"}), "-ERR unknown command 'nope', with args beginning with: \r\n");  // unknown first
}

TEST(Registry, FlagNamesAndArity) {
  EXPECT_EQ(FlagNames(kCmdWrite | kCmdFast), (std::vector<std::string_view>{"write", "fast"}));
  EXPECT_TRUE(ArityOk(2, 2));
  EXPECT_FALSE(ArityOk(2, 3));
  EXPECT_TRUE(ArityOk(-2, 5));
  EXPECT_FALSE(ArityOk(-2, 1));
}

// COMMAND INFO on live redis:7.2.5 (M6b plan "Sources"); TidesKV adds loading to replicaof/slaveof (D7).
TEST(RegistryFlags, LoadingAndStaleMatchRedis72) {
  CommandRegistry r;
  RegisterAllCommands(r);
  // clusterx, cluster: allowed while loading/stale so a full-syncing replica still answers the controller
  // (M7a D12).
  const std::set<std::string> loading = {
      "auth", "client", "cluster",  "command", "config", "debug",   "discard",  "clusterx",  "echo",
      "exec", "hello",  "info",     "multi",   "psync",  "quit",    "replconf", "replicaof", "reset",
      "role", "select", "shutdown", "slaveof", "sync",   "unwatch", "watch"};
  const std::set<std::string> stale = {
      "auth",     "client",    "cluster", "clusterx",   "command", "config",   "debug",   "discard", "echo",
      "eval",     "eval_ro",   "evalsha", "evalsha_ro", "exec",    "hello",    "info",    "multi",   "quit",
      "replconf", "replicaof", "reset",   "role",       "select",  "shutdown", "slaveof", "unwatch", "watch"};
  for (const auto& [name, spec] : r.all()) {
    EXPECT_EQ((spec.flags & kCmdLoading) != 0, loading.contains(name)) << name;
    EXPECT_EQ((spec.flags & kCmdStale) != 0, stale.contains(name)) << name;
  }
}

TEST(RegistryFlags, EffectiveFlagsOfScriptsAndScriptSubcommands) {
  test::CommandHarness h;
  resp::ReplyBuilder reply(2);
  ExecContext ctx{h.session, reply, h.services};
  const auto flags = [&](const ArgList& argv) {
    return EffectiveFlags(ctx, *h.services.registry().Find(argv[0]), argv);
  };
  EXPECT_EQ(flags({"EVAL", "return 1", "0"}) & (kCmdStale | kCmdWrite), 0u);  // compat: not stale (live)
  EXPECT_EQ(flags({"EVAL", "#!lua\nreturn 1", "0"}) & (kCmdStale | kCmdWrite), kCmdWrite);
  EXPECT_EQ(flags({"EVAL", "#!lua flags=no-writes,allow-stale\nreturn 1", "0"}) & (kCmdStale | kCmdWrite),
            kCmdStale);
  EXPECT_NE(flags({"EVALSHA", std::string(40, 'f'), "0"}) & kCmdStale, 0u);  // unknown sha: as registered
  EXPECT_EQ(flags({"SCRIPT", "HELP"}) & (kCmdLoading | kCmdStale), kCmdLoading | kCmdStale);
  EXPECT_EQ(flags({"SCRIPT", "LOAD", "return 1"}) & (kCmdLoading | kCmdStale), kCmdStale);
  EXPECT_EQ(flags({"SCRIPT", "EXISTS", "x"}) & (kCmdLoading | kCmdStale), 0u);
}

TEST(RegistryFlags, ExecInheritsItsQueue) {
  test::CommandHarness h;
  const CommandSpec& exec = *h.services.registry().Find("exec");
  EXPECT_EQ(h.Run({"MULTI"}), test::kOk);
  EXPECT_EQ(h.Run({"PING"}), "+QUEUED\r\n");
  CallFlags f = DeriveCallFlags(h.session, exec, exec.flags);
  EXPECT_FALSE(f.is_write);
  EXPECT_TRUE(f.deny_stale);    // PING is not STALE
  EXPECT_TRUE(f.deny_loading);  // nor LOADING
  EXPECT_EQ(h.Run({"DISCARD"}), test::kOk);
  EXPECT_EQ(h.Run({"MULTI"}), test::kOk);
  EXPECT_EQ(h.Run({"INFO", "server"}), "+QUEUED\r\n");
  f = DeriveCallFlags(h.session, exec, exec.flags);
  EXPECT_FALSE(f.deny_stale);
  EXPECT_FALSE(f.deny_loading);
  EXPECT_EQ(h.Run({"SET", "k", "v"}), "+QUEUED\r\n");
  f = DeriveCallFlags(h.session, exec, exec.flags);
  EXPECT_TRUE(f.is_write);
  EXPECT_TRUE(f.deny_stale);
  EXPECT_EQ(h.Run({"DISCARD"}), test::kOk);
  EXPECT_FALSE(h.session.multi.has_non_stale);
  EXPECT_FALSE(h.session.multi.has_non_loading);
}

TEST(RegistryFlags, CommandInfoShowsLoadingAndStale) {
  test::CommandHarness h;
  const std::string info = h.Run({"COMMAND", "INFO", "info"});
  EXPECT_NE(info.find("loading"), std::string::npos) << info;
  EXPECT_NE(info.find("stale"), std::string::npos) << info;
  EXPECT_EQ(h.Run({"COMMAND", "INFO", "get"}).find("loading"), std::string::npos);
}

}  // namespace
}  // namespace tideskv
