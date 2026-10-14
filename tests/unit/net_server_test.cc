#include "net/server.h"

#include <gtest/gtest.h>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "server/blocking.h"
#include "server/connection_block_handler.h"
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
  } else if (argv[0] == "PARTIAL_BOOM") {
    r.ArrayHeader(2);  // half-written reply before the failure
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
  auto r1 = c->Read();
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1->str, "2");
  auto r2 = c->Read();
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2->str, "20");

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

TEST(NetServer, ThrowAfterPartialReplyDropsPartialBytes) {
  auto server = StartServer();
  auto c = RespClient::Connect(server->port());
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->SendRaw(test::EncodeCommand({"ok"}) + test::EncodeCommand({"PARTIAL_BOOM"})));
  auto first = c->Read();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->str, "ok");  // earlier pipelined replies survive
  auto r = c->Read();
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->type, RespValue::Type::kError);  // not the stray "*2" array header
  EXPECT_EQ(r->str, "ERR internal error");
  EXPECT_TRUE(c->WaitForClose());
}

TEST(NetServer, StartTwiceFails) {
  auto server = StartServer();
  EXPECT_EQ(server->Start().code(), StatusCode::kInvalidArgument);
  auto c = RespClient::Connect(server->port());
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"ok"}).str, "ok");
}

// Executor for the blocking tests: BLOCK <ms> parks the command (0 = forever); WAKE <id> <text> unblocks
// client <id> with +<text>; WAITERS counts blocked clients; anything else echoes.
std::unique_ptr<Server> StartBlockingServer(BlockingRegistry& registry) {
  ServerOptions o;
  o.port = 0;
  o.workers = 2;
  auto exec = [&registry](Session& s, const std::vector<std::string>& argv, resp::ReplyBuilder& r) {
    if (argv[0] == "BLOCK") {
      ReadyKeys ready;
      ConnectionBlockHandler block(registry, s, argv, ready);
      block.Block({encoding::ValueType::kList, {"k"}, std::stoll(argv[1])});
    } else if (argv[0] == "WAKE") {
      auto c = registry.Find(std::stoull(argv[1]));
      r.Integer(c != nullptr && c->Unblock("+" + argv[2] + "\r\n") ? 1 : 0);
    } else if (argv[0] == "DISCONNECTALL") {
      r.Integer(static_cast<int64_t>(registry.DisconnectAll("UNBLOCKED " + argv[1])));
    } else if (argv[0] == "WAITERS") {
      r.Integer(static_cast<int64_t>(registry.blocked_clients()));
    } else if (argv[0] == "ID") {
      r.Integer(static_cast<int64_t>(s.id));
    } else {
      r.Bulk(argv[0]);
    }
  };
  auto server = std::make_unique<Server>(std::move(o), exec);
  EXPECT_TRUE(server->Start().ok());
  return server;
}

TEST(NetServerBlocking, TimeoutRepliesNullArrayThenRunsThePipeline) {  // Review Focus #4
  BlockingRegistry registry;
  auto server = StartBlockingServer(registry);
  auto c = RespClient::Connect(server->port());
  ASSERT_NE(c, nullptr);
  const auto start = std::chrono::steady_clock::now();
  ASSERT_TRUE(c->SendRaw(test::EncodeCommand({"BLOCK", "150"}) + test::EncodeCommand({"AFTER"})));
  auto first = c->Read();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->type, RespValue::Type::kNull);  // *-1
  EXPECT_GE(std::chrono::steady_clock::now() - start, 140ms);
  auto second = c->Read();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->str, "AFTER");  // the pipelined command ran only after the timeout
  EXPECT_EQ(registry.blocked_clients(), 0u);
}

TEST(NetServerBlocking, UnblockFromAnotherConnectionDeliversInOrder) {
  BlockingRegistry registry;
  auto server = StartBlockingServer(registry);
  auto a = RespClient::Connect(server->port());
  auto b = RespClient::Connect(server->port());
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  const int64_t id = a->Command({"ID"}).integer;
  ASSERT_TRUE(a->SendRaw(test::EncodeCommand({"BLOCK", "0"}) + test::EncodeCommand({"NEXT"})));
  ASSERT_TRUE(WaitFor([&] { return b->Command({"WAITERS"}).integer == 1; }));
  EXPECT_EQ(b->Command({"WAKE", std::to_string(id), "served"}).integer, 1);
  auto r1 = a->Read();
  auto r2 = a->Read();
  ASSERT_TRUE(r1 && r2);
  EXPECT_EQ(r1->str, "served");
  EXPECT_EQ(r2->str, "NEXT");
  EXPECT_EQ(b->Command({"WAKE", std::to_string(id), "again"}).integer, 0);  // no longer blocked
}

// M6a D14: the error is written, the pipelined command behind the block never runs, the link closes.
TEST(NetServerBlocking, DisconnectAllRepliesThenCloses) {
  BlockingRegistry registry;
  auto server = StartBlockingServer(registry);
  auto a = RespClient::Connect(server->port());
  auto b = RespClient::Connect(server->port());
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_TRUE(a->SendRaw(test::EncodeCommand({"BLOCK", "0"}) + test::EncodeCommand({"NEXT"})));
  ASSERT_TRUE(WaitFor([&] { return b->Command({"WAITERS"}).integer == 1; }));
  EXPECT_EQ(b->Command({"DISCONNECTALL", "gone"}).integer, 1);
  auto r1 = a->Read();
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1->type, RespValue::Type::kError);
  EXPECT_EQ(r1->str, "UNBLOCKED gone");
  EXPECT_TRUE(a->WaitForClose());
  EXPECT_EQ(b->Command({"PING"}).str, "PING");  // other clients are untouched
}

TEST(NetServerBlocking, PipelinedBlocksRunOneAfterTheOther) {  // Review Focus #2
  BlockingRegistry registry;
  auto server = StartBlockingServer(registry);
  auto a = RespClient::Connect(server->port());
  auto b = RespClient::Connect(server->port());
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  const int64_t id = a->Command({"ID"}).integer;
  ASSERT_TRUE(a->SendRaw(test::EncodeCommand({"BLOCK", "0"}) + test::EncodeCommand({"BLOCK", "0"})));
  ASSERT_TRUE(WaitFor([&] { return b->Command({"WAITERS"}).integer == 1; }));
  EXPECT_EQ(b->Command({"WAKE", std::to_string(id), "one"}).integer, 1);
  auto r1 = a->Read();
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1->str, "one");
  ASSERT_TRUE(WaitFor([&] { return b->Command({"WAITERS"}).integer == 1; }));  // the second one now waits
  EXPECT_EQ(b->Command({"WAKE", std::to_string(id), "two"}).integer, 1);
  auto r2 = a->Read();
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2->str, "two");
}

TEST(NetServerBlocking, DisconnectWhileBlockedCancelsTheWait) {  // Review Focus #4
  BlockingRegistry registry;
  auto server = StartBlockingServer(registry);
  auto a = RespClient::Connect(server->port());
  ASSERT_NE(a, nullptr);
  ASSERT_TRUE(a->Send({"BLOCK", "0"}));
  ASSERT_TRUE(WaitFor([&] { return registry.blocked_clients() == 1; }));
  a->Close();
  EXPECT_TRUE(WaitFor([&] { return registry.blocked_clients() == 0; }));
}

TEST(NetServerBlocking, StopWithABlockedClientIsPrompt) {
  BlockingRegistry registry;
  auto server = StartBlockingServer(registry);
  auto a = RespClient::Connect(server->port());
  ASSERT_NE(a, nullptr);
  ASSERT_TRUE(a->Send({"BLOCK", "0"}));
  ASSERT_TRUE(WaitFor([&] { return registry.blocked_clients() == 1; }));
  const auto start = std::chrono::steady_clock::now();
  server->Stop();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
  EXPECT_EQ(registry.blocked_clients(), 0u);  // destroying the connection cancelled the wait
}

TEST(NetServerBlocking, ManyRepliesWhileReadingStayOrdered) {
  BlockingRegistry registry;
  auto server = StartBlockingServer(registry);
  auto c = RespClient::Connect(server->port());
  ASSERT_NE(c, nullptr);
  std::string batch;
  for (int i = 0; i < 2000; ++i) batch += test::EncodeCommand({"x" + std::to_string(i)});
  ASSERT_TRUE(c->SendRaw(batch));
  for (int i = 0; i < 2000; ++i) {
    auto r = c->Read();
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->str, "x" + std::to_string(i));
  }
}

TEST(NetServer, OnDisconnectRunsOncePerConnection) {
  std::atomic<int> calls{0};
  std::atomic<uint64_t> last_id{0};
  ServerOptions o;
  o.on_disconnect = [&](Session& s) {
    last_id = s.id;
    ++calls;
  };
  auto server = StartServer(std::move(o));
  auto c = RespClient::Connect(server->port());
  ASSERT_NE(c, nullptr);
  const int64_t id = c->Command({"ID"}).integer;
  c->Close();
  EXPECT_TRUE(WaitFor([&] { return calls.load() == 1; }));
  EXPECT_EQ(last_id.load(), static_cast<uint64_t>(id));
  server->Stop();
  EXPECT_EQ(calls.load(), 1);
}

// Takeover executor: PSYNC replies +CONTINUE and asks for the socket; BLOCK <ms> parks; others echo.
struct Takeover {
  std::mutex mu;
  std::condition_variable cv;
  int fd = -1;
  std::string unread;
  bool done = false;

  bool Wait(std::chrono::milliseconds timeout = 2s) {
    std::unique_lock lock(mu);
    return cv.wait_for(lock, timeout, [&] { return done; });
  }
};

std::unique_ptr<Server> StartTakeoverServer(Takeover& t, BlockingRegistry& registry) {
  ServerOptions o;
  o.port = 0;
  o.workers = 1;
  auto exec = [&t, &registry](Session& s, const std::vector<std::string>& argv, resp::ReplyBuilder& r) {
    if (argv[0] == "PSYNC") {
      r.SimpleString("CONTINUE");
      s.takeover = [&t](int fd, std::string unread) {
        std::lock_guard lock(t.mu);
        t.fd = fd;
        t.unread = std::move(unread);
        t.done = true;
        t.cv.notify_all();
      };
    } else if (argv[0] == "BLOCK") {
      ReadyKeys ready;
      ConnectionBlockHandler block(registry, s, argv, ready);
      block.Block({encoding::ValueType::kList, {"k"}, std::stoll(argv[1])});
    } else {
      r.Bulk(argv[0]);
    }
  };
  auto server = std::make_unique<Server>(std::move(o), exec);
  EXPECT_TRUE(server->Start().ok());
  return server;
}

// Reads from a raw fd until `n` bytes arrive or the timeout passes.
std::string ReadRaw(int fd, size_t n, std::chrono::milliseconds timeout = 2s) {
  std::string out;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (out.size() < n && std::chrono::steady_clock::now() < deadline) {
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, 20) <= 0) continue;
    char buf[4096];
    const ssize_t got = ::recv(fd, buf, sizeof(buf), 0);
    if (got <= 0) break;
    out.append(buf, static_cast<size_t>(got));
  }
  return out;
}

TEST(NetServerTakeover, HandsTheSocketAndTheUnreadBytesOver) {
  Takeover t;
  BlockingRegistry registry;
  auto server = StartTakeoverServer(t, registry);
  auto c = RespClient::Connect(server->port(), 300ms);
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(WaitFor([&] { return server->counters().connected == 1; }));
  const std::string rest = test::EncodeCommand({"REPLCONF", "ACK", "1"}) + test::EncodeCommand({"ECHO"});
  ASSERT_TRUE(c->SendRaw(test::EncodeCommand({"PSYNC"}) + rest));
  auto first = c->Read();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->str, "CONTINUE");
  EXPECT_FALSE(c->Read().has_value());  // nothing after PSYNC is answered: it belongs to the stream
  ASSERT_TRUE(t.Wait());
  EXPECT_EQ(t.unread, rest);
  ASSERT_GE(t.fd, 0);
  EXPECT_TRUE(WaitFor([&] { return server->counters().connected == 0; }));  // no longer a client
  ASSERT_TRUE(c->SendRaw("late"));
  EXPECT_EQ(ReadRaw(t.fd, 4), "late");  // the new owner reads the socket
  const std::string hello = "+hello\r\n";
  ASSERT_EQ(::send(t.fd, hello.data(), hello.size(), MSG_NOSIGNAL), static_cast<ssize_t>(hello.size()));
  auto pushed = c->Read();
  ASSERT_TRUE(pushed.has_value());
  EXPECT_EQ(pushed->str, "hello");
  ::close(t.fd);
  EXPECT_TRUE(c->WaitForClose());
}

// After a blocking command a read is in flight when PSYNC runs: the hand-off must cancel it without
// losing bytes, and the socket must survive the cancelled read's completion.
TEST(NetServerTakeover, HandoffWithAReadInFlightLosesNoBytes) {
  Takeover t;
  BlockingRegistry registry;
  auto server = StartTakeoverServer(t, registry);
  auto c = RespClient::Connect(server->port(), 300ms);
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->SendRaw(test::EncodeCommand({"BLOCK", "50"}) + test::EncodeCommand({"PSYNC"})));
  auto timed_out = c->Read();
  ASSERT_TRUE(timed_out.has_value());
  EXPECT_EQ(timed_out->type, RespValue::Type::kNull);
  auto cont = c->Read();
  ASSERT_TRUE(cont.has_value());
  EXPECT_EQ(cont->str, "CONTINUE");
  ASSERT_TRUE(t.Wait());
  EXPECT_EQ(t.unread, "");
  ASSERT_TRUE(c->SendRaw("after"));
  EXPECT_EQ(ReadRaw(t.fd, 5), "after");
  ::close(t.fd);
}

}  // namespace
}  // namespace tideskv::net
