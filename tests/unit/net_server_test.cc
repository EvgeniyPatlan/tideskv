#include "net/server.h"

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <thread>

#include "support/resp_client.h"

namespace tideskv::net {
namespace {

using test::RespClient;
using test::RespValue;
using namespace std::chrono_literals;

// Test executor: echoes argv joined by spaces; ID/NAME expose session fields; QUIT closes.
void TestExecutor(Session& s, const std::vector<std::string>& argv, resp::ReplyBuilder& r) {
  if (argv[0] == "BOOM") {
    throw std::runtime_error("boom");
  } else if (argv[0] == "QUIT") {
    r.SimpleString("OK");
    s.close_after_reply = true;
  } else if (argv[0] == "ID") {
    r.Integer(static_cast<int64_t>(s.id));
  } else if (argv[0] == "NAME") {
    r.Bulk(s.name);
  } else {
    std::string joined;
    for (const auto& a : argv) joined += (joined.empty() ? "" : " ") + a;
    r.Bulk(joined);
  }
}

std::unique_ptr<Server> StartServer(ServerOptions o = {}) {
  o.port = 0;
  o.workers = 2;
  auto server = std::make_unique<Server>(std::move(o), &TestExecutor);
  const Status st = server->Start();
  EXPECT_TRUE(st.ok()) << st.ToString();
  return server;
}

bool WaitFor(const std::function<bool()>& cond, std::chrono::milliseconds timeout = 2s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (cond()) return true;
    std::this_thread::sleep_for(10ms);
  }
  return cond();
}

TEST(NetServer, SingleCommandRoundTrip) {
  auto server = StartServer();
  auto c = RespClient::Connect(server->port());
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"hello", "world"}).str, "hello world");
}

TEST(NetServer, PipelinedCommandsAnsweredInOrder) {
  auto server = StartServer();
  auto c = RespClient::Connect(server->port());
  ASSERT_NE(c, nullptr);
  std::string batch;
  for (int i = 0; i < 100; ++i) batch += test::EncodeCommand({"c", std::to_string(i)});
  ASSERT_TRUE(c->SendRaw(batch));
  for (int i = 0; i < 100; ++i) {
    auto r = c->Read();
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->str, "c " + std::to_string(i));
  }
}

TEST(NetServer, ByteByByteCommandIsParsed) {  // Review Focus #1
  auto server = StartServer();
  auto c = RespClient::Connect(server->port());
  ASSERT_NE(c, nullptr);
  const std::string wire = test::EncodeCommand({"frag", std::string("x\r\ny", 4)});
  for (char ch : wire) {
    ASSERT_TRUE(c->SendRaw(std::string_view(&ch, 1)));
    std::this_thread::sleep_for(1ms);
  }
  auto r = c->Read();
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->str, std::string("frag x\r\ny", 9));
}

TEST(NetServer, ProtocolErrorRepliesThenCloses) {  // Review Focus #2
  auto server = StartServer();
  auto c = RespClient::Connect(server->port());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->SendRaw("*1\r\n$-5\r\n"));
  auto r = c->Read();
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->type, RespValue::Type::kError);
  EXPECT_EQ(r->str, "ERR Protocol error: invalid bulk length");
  EXPECT_TRUE(c->WaitForClose());
}

TEST(NetServer, UnauthenticatedLimitsApplyUntilAuth) {
  ServerOptions o;
  o.init_session = [](Session& s) { s.authenticated = false; };
  o.port = 0;
  o.workers = 2;
  auto server = std::make_unique<Server>(
      std::move(o), [](Session& s, const std::vector<std::string>& argv, resp::ReplyBuilder& r) {
        if (argv[0] == "AUTH") s.authenticated = true;
        r.Bulk(std::to_string(argv.size()));
      });
  ASSERT_TRUE(server->Start().ok());

  auto c = RespClient::Connect(server->port());
  ASSERT_NE(c, nullptr);
  // AUTH and a 20-arg command pipelined: the limit must switch per command.
  std::vector<std::string> wide(20, "a");
  ASSERT_TRUE(c->SendRaw(test::EncodeCommand({"AUTH", "pw"}) + test::EncodeCommand(wide)));
  EXPECT_EQ(c->Read()->str, "2");
  EXPECT_EQ(c->Read()->str, "20");

  auto u = RespClient::Connect(server->port());
  ASSERT_NE(u, nullptr);
  ASSERT_TRUE(u->SendRaw(test::EncodeCommand(std::vector<std::string>(11, "a"))));
  auto r = u->Read();
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->type, RespValue::Type::kError);
  EXPECT_EQ(r->str, "ERR Protocol error: unauthenticated multibulk length");
  EXPECT_TRUE(u->WaitForClose());
}

TEST(NetServer, QuitClosesAfterReply) {
  auto server = StartServer();
  auto c = RespClient::Connect(server->port());
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"QUIT"}).str, "OK");
  EXPECT_TRUE(c->WaitForClose());
}

TEST(NetServer, MaxClientsRejectsExtraConnections) {
  ServerOptions o;
  o.maxclients = [] { return 1; };
  auto server = StartServer(std::move(o));
  auto c1 = RespClient::Connect(server->port());
  ASSERT_NE(c1, nullptr);
  EXPECT_EQ(c1->Command({"a"}).str, "a");
  auto c2 = RespClient::Connect(server->port());
  ASSERT_NE(c2, nullptr);
  auto r = c2->Read();
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->str, "ERR max number of clients reached");
  EXPECT_TRUE(c2->WaitForClose());
}

TEST(NetServer, QueryBufferLimitClosesConnection) {
  ServerOptions o;
  o.query_buffer_limit = [] { return size_t{1024}; };
  auto server = StartServer(std::move(o));
  auto c = RespClient::Connect(server->port());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->SendRaw("*1\r\n$100000\r\n" + std::string(4096, 'x')));
  EXPECT_TRUE(c->WaitForClose());
}

TEST(NetServer, AbruptDisconnectMidCommandIsHarmless) {  // Review Focus #4
  auto server = StartServer();
  {
    auto c = RespClient::Connect(server->port());
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(c->SendRaw("*2\r\n$3\r\nfoo\r\n$100\r\nabc"));
  }  // closed mid-command
  auto c2 = RespClient::Connect(server->port());
  ASSERT_NE(c2, nullptr);
  EXPECT_EQ(c2->Command({"still", "alive"}).str, "still alive");
  EXPECT_TRUE(WaitFor([&] { return server->counters().connected == 1; }));
}

TEST(NetServer, StopWithLiveConnectionsIsPrompt) {  // Review Focus #4
  auto server = StartServer();
  std::vector<std::unique_ptr<RespClient>> clients;
  for (int i = 0; i < 50; ++i) {
    clients.push_back(RespClient::Connect(server->port()));
    ASSERT_NE(clients.back(), nullptr);
  }
  ASSERT_TRUE(clients[0]->SendRaw("*2\r\n$3\r\nfoo\r\n"));  // one connection mid-command
  ASSERT_TRUE(WaitFor([&] { return server->counters().connected == 50; }));
  const auto start = std::chrono::steady_clock::now();
  server->Stop();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
  EXPECT_TRUE(clients[1]->WaitForClose());
  server->Stop();  // idempotent
}

TEST(NetServer, CountersAndSessionInit) {
  ServerOptions o;
  o.init_session = [](Session& s) { s.name = "init"; };
  auto server = StartServer(std::move(o));
  auto c = RespClient::Connect(server->port());
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"NAME"}).str, "init");
  EXPECT_GE(c->Command({"ID"}).integer, 1);
  const ServerCounters k = server->counters();
  EXPECT_EQ(k.total_connections, 1u);
  EXPECT_EQ(k.total_commands, 2u);
  server->ResetCounters();
  EXPECT_EQ(server->counters().total_commands, 0u);
  EXPECT_EQ(server->counters().connected, 1u);
}

TEST(NetServer, PortInUseFailsStart) {
  auto first = StartServer();
  ServerOptions o;
  o.port = first->port();
  Server second(std::move(o), &TestExecutor);
  EXPECT_EQ(second.Start().code(), StatusCode::kIOError);
}

TEST(NetServer, BadBindAddressFailsStart) {
  ServerOptions o;
  o.bind = "not-an-ip";
  o.port = 0;
  Server server(std::move(o), &TestExecutor);
  EXPECT_EQ(server.Start().code(), StatusCode::kInvalidArgument);
}

TEST(NetServer, ExecutorExceptionRepliesErrorAndCloses) {
  auto server = StartServer();
  auto c = RespClient::Connect(server->port());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->Send({"BOOM"}));
  auto r = c->Read();
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->type, RespValue::Type::kError);
  EXPECT_EQ(r->str, "ERR internal error");
  EXPECT_TRUE(c->WaitForClose());
  auto c2 = RespClient::Connect(server->port());
  ASSERT_NE(c2, nullptr);
  EXPECT_EQ(c2->Command({"alive"}).str, "alive");
}

TEST(NetServer, StartTwiceFails) {
  auto server = StartServer();
  EXPECT_EQ(server->Start().code(), StatusCode::kInvalidArgument);
  auto c = RespClient::Connect(server->port());
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"ok"}).str, "ok");
}

}  // namespace
}  // namespace tideskv::net
