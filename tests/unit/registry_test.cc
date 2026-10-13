#include "commands/registry.h"

#include <gtest/gtest.h>

#include <stdexcept>

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

}  // namespace
}  // namespace tideskv
