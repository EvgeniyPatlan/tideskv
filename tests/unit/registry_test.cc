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
