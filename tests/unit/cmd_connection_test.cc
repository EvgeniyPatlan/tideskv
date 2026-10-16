#include <gtest/gtest.h>

#include <string>

#include "support/command_harness.h"
#include "types/errors.h"

namespace tideskv {
namespace {

using test::CommandHarness;

TEST(Ping, Variants) {
  CommandHarness h;
  EXPECT_EQ(h.Run({"PING"}), "+PONG\r\n");
  EXPECT_EQ(h.Run({"ping", "hello"}), "$5\r\nhello\r\n");
  EXPECT_EQ(h.Run({"PING", "a", "b"}), "-ERR wrong number of arguments for 'ping' command\r\n");
}

TEST(Echo, BinarySafe) {
  CommandHarness h;
  EXPECT_EQ(h.Run({"ECHO", std::string("a\0\r\n", 4)}), std::string("$4\r\na\0\r\n\r\n", 10));
}

TEST(Quit, RepliesOkAndCloses) {
  CommandHarness h;
  EXPECT_EQ(h.Run({"QUIT"}), "+OK\r\n");
  EXPECT_TRUE(h.session.close_after_reply);
}

TEST(Auth, WithoutConfiguredPassword) {
  CommandHarness h;
  EXPECT_NE(h.Run({"AUTH", "x"}).find("without any password configured"), std::string::npos);
  EXPECT_EQ(h.Run({"AUTH", "default", "anything"}), "+OK\r\n");
  EXPECT_EQ(h.Run({"AUTH", "bob", "x"}),
            "-WRONGPASS invalid username-password pair or user is disabled.\r\n");
}

TEST(Auth, PasswordFlow) {
  CommandHarness h;
  h.RequirePassword("s3cret");
  EXPECT_EQ(h.Run({"PING"}), "-NOAUTH Authentication required.\r\n");
  EXPECT_EQ(h.Run({"AUTH", "wrong"}), "-WRONGPASS invalid username-password pair or user is disabled.\r\n");
  EXPECT_FALSE(h.session.authenticated);
  EXPECT_EQ(h.Run({"AUTH", "s3cret"}), "+OK\r\n");
  EXPECT_TRUE(h.session.authenticated);
  EXPECT_EQ(h.Run({"PING"}), "+PONG\r\n");
  EXPECT_EQ(h.Run({"AUTH", "default", "s3cret"}), "+OK\r\n");
  EXPECT_EQ(h.Run({"AUTH", "other", "s3cret"}),
            "-WRONGPASS invalid username-password pair or user is disabled.\r\n");
}

TEST(Auth, SyntaxError) {
  CommandHarness h;
  EXPECT_EQ(h.Run({"AUTH", "a", "b", "c"}), "-ERR syntax error\r\n");
}

TEST(Hello, DefaultsToResp2Map) {
  CommandHarness h;
  const std::string r = h.Run({"HELLO"});
  EXPECT_EQ(r.rfind("*14\r\n$6\r\nserver\r\n$5\r\nredis\r\n$7\r\nversion\r\n$5\r\n7.2.0\r\n", 0), 0u) << r;
  EXPECT_NE(r.find("$5\r\nproto\r\n:2\r\n$2\r\nid\r\n:7\r\n"), std::string::npos) << r;
  EXPECT_EQ(h.session.protocol, 2);
}

TEST(Hello, SwitchesToResp3) {
  CommandHarness h;
  const std::string r = h.Run({"HELLO", "3"});
  EXPECT_EQ(r.rfind("%7\r\n", 0), 0u) << r;
  EXPECT_NE(r.find("$7\r\nmodules\r\n*0\r\n"), std::string::npos) << r;
  EXPECT_EQ(h.session.protocol, 3);
  EXPECT_EQ(h.reply.protocol(), 3);
}

TEST(Hello, RejectsBadProtocol) {
  CommandHarness h;
  EXPECT_EQ(h.Run({"HELLO", "4"}), "-NOPROTO unsupported protocol version\r\n");
  EXPECT_EQ(h.Run({"HELLO", "abc"}), "-ERR Protocol version is not an integer or out of range\r\n");
  EXPECT_EQ(h.Run({"HELLO", "3", "BOGUS"}), "-ERR Syntax error in HELLO option 'BOGUS'\r\n");
  EXPECT_EQ(h.session.protocol, 2);
}

TEST(Hello, AuthAndSetname) {
  CommandHarness h;
  h.RequirePassword("s3cret");
  EXPECT_EQ(h.Run({"HELLO", "3"}).rfind("-NOAUTH HELLO must be called", 0), 0u);
  EXPECT_EQ(h.Run({"HELLO", "3", "AUTH", "default", "bad"}).rfind("-WRONGPASS", 0), 0u);
  EXPECT_EQ(h.Run({"HELLO", "3", "AUTH", "default", "s3cret", "SETNAME", "conn1"}).rfind("%7\r\n", 0), 0u);
  EXPECT_TRUE(h.session.authenticated);
  EXPECT_EQ(h.session.name, "conn1");
}

TEST(Hello, SyntaxErrorAfterAuth) {
  CommandHarness h;
  h.RequirePassword("s3cret");
  const std::string r = h.Run({"HELLO", "3", "AUTH", "default", "s3cret", "BOGUS"});
  EXPECT_EQ(r.rfind("-ERR Syntax error in HELLO option 'BOGUS'", 0), 0u);
  EXPECT_FALSE(h.session.authenticated);
}

TEST(Hello, AuthErrorPreservesUnauthenticated) {
  CommandHarness h;
  h.RequirePassword("s3cret");
  const std::string r = h.Run({"HELLO", "3", "AUTH", "default", "s3cret", "AUTH", "default", "wrong"});
  EXPECT_EQ(r.rfind("-WRONGPASS", 0), 0u);
  EXPECT_FALSE(h.session.authenticated);
}

TEST(Hello, InvalidSetnameWithoutAuth) {
  CommandHarness h;
  h.RequirePassword("s3cret");
  const std::string r = h.Run({"HELLO", "3", "SETNAME", "bad name"});
  EXPECT_EQ(r.rfind("-NOAUTH HELLO must be called", 0), 0u);
}

TEST(Client, IdNamesAndInfo) {
  CommandHarness h;
  EXPECT_EQ(h.Run({"CLIENT", "ID"}), ":7\r\n");
  EXPECT_EQ(h.Run({"CLIENT", "GETNAME"}), "$-1\r\n");
  EXPECT_EQ(h.Run({"CLIENT", "SETNAME", "bad name"}),
            "-ERR Client names cannot contain spaces, newlines or special characters.\r\n");
  EXPECT_EQ(h.Run({"CLIENT", "SETNAME", "worker-1"}), "+OK\r\n");
  EXPECT_EQ(h.Run({"client", "getname"}), "$8\r\nworker-1\r\n");
  EXPECT_EQ(h.Run({"CLIENT", "SETINFO", "LIB-NAME", "redis-py"}), "+OK\r\n");
  EXPECT_EQ(h.session.lib_name, "redis-py");
  EXPECT_EQ(h.Run({"CLIENT", "SETINFO", "COLOR", "x"}), "-ERR Unrecognized option 'COLOR'\r\n");
  EXPECT_EQ(h.Run({"CLIENT", "FOO"}),
            "-ERR unknown subcommand or wrong number of arguments for 'FOO'. Try CLIENT HELP.\r\n");
}

TEST(ClientUnblock, TimeoutAndErrorReasons) {  // Review Focus #4
  test::CommandHarness h;
  test::SecondClient other(h);
  const std::string id = std::to_string(other.session.id);
  EXPECT_EQ(other.Run({"BLPOP", "k", "0"}), "");
  EXPECT_EQ(h.Run({"CLIENT", "UNBLOCK", id, "BOGUS"}), test::Err(kErrUnblockReason));
  EXPECT_EQ(h.Run({"CLIENT", "UNBLOCK", "x"}), test::Err(kErrNotInteger));
  EXPECT_EQ(h.Run({"CLIENT", "UNBLOCK", "999"}), test::Int(0));
  EXPECT_EQ(h.Run({"CLIENT", "UNBLOCK", id}), test::Int(1));  // TIMEOUT is the default
  EXPECT_EQ(other.TakeUnblocked(), (std::vector<std::string>{"*-1\r\n"}));
  EXPECT_EQ(h.Run({"CLIENT", "UNBLOCK", id}), test::Int(0));
  EXPECT_EQ(other.Run({"BLPOP", "k", "0"}), "");
  EXPECT_EQ(h.Run({"CLIENT", "unblock", id, "error"}), test::Int(1));
  EXPECT_EQ(other.TakeUnblocked(), (std::vector<std::string>{test::Err(kErrUnblockedByClient)}));
}

TEST(ClientUnblock, TimeoutReplyUsesTheTargetsProtocol) {
  test::CommandHarness h;
  test::SecondClient other(h);
  other.session.protocol = 3;
  const std::string id = std::to_string(other.session.id);
  EXPECT_EQ(other.Run({"BLPOP", "k", "0"}), "");
  EXPECT_EQ(h.Run({"CLIENT", "UNBLOCK", id, "TIMEOUT"}), test::Int(1));
  EXPECT_EQ(other.TakeUnblocked(), (std::vector<std::string>{"_\r\n"}));
}

TEST(Hello, ReportsClusterModeWhenEnabled) {  // Kvrocks cmd_server.cc:1018-1024
  CommandHarness h;
  EXPECT_NE(h.Run({"HELLO"}).find("$4\r\nmode\r\n$10\r\nstandalone\r\n"), std::string::npos);
  ASSERT_TRUE(h.services.config_.Set("cluster-enabled", "yes", /*at_runtime=*/false).ok());
  EXPECT_NE(h.Run({"HELLO"}).find("$4\r\nmode\r\n$7\r\ncluster\r\n"), std::string::npos);
}

}  // namespace
}  // namespace tideskv
