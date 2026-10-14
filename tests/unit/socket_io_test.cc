#include "repl/socket_io.h"

#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace tideskv::repl {
namespace {

using namespace std::chrono_literals;

struct Pair {
  Pair() {
    int fds[2] = {-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    a = Fd(fds[0]);
    b = Fd(fds[1]);
  }
  Fd a;
  Fd b;
};

// A listening localhost socket on an ephemeral port.
Fd Listen(uint16_t* port) {
  Fd fd(::socket(AF_INET, SOCK_STREAM, 0));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  EXPECT_EQ(::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
  EXPECT_EQ(::listen(fd.get(), 4), 0);
  socklen_t len = sizeof(addr);
  EXPECT_EQ(::getsockname(fd.get(), reinterpret_cast<sockaddr*>(&addr), &len), 0);
  *port = ntohs(addr.sin_port);
  return fd;
}

TEST(SocketIo, FdIsMoveOnlyAndClosesOnce) {
  Pair p;
  const int raw = p.a.get();
  Fd moved(std::move(p.a));
  EXPECT_EQ(p.a.get(), -1);
  EXPECT_EQ(moved.get(), raw);
  Fd assigned;
  assigned = std::move(moved);
  EXPECT_EQ(assigned.get(), raw);
  const int released = assigned.release();
  EXPECT_EQ(released, raw);
  EXPECT_EQ(assigned.get(), -1);
  EXPECT_EQ(::close(released), 0);  // still open: release() handed it over
}

TEST(SocketIo, WriteAllThenReadSomeRoundTrip) {
  Pair p;
  const std::string big(size_t{1} << 20, 'x');  // larger than the socket buffer: needs several sends
  std::string got;
  std::thread reader([&] {
    while (got.size() < big.size()) {
      if (!ReadSome(p.b.get(), &got, 2s).ok()) break;
    }
  });
  EXPECT_TRUE(WriteAll(p.a.get(), big, 2s).ok());
  reader.join();
  EXPECT_EQ(got, big);
}

TEST(SocketIo, ReadSomeAppends) {
  Pair p;
  ASSERT_TRUE(WriteAll(p.a.get(), "world", 1s).ok());
  std::string out = "hello ";
  ASSERT_TRUE(ReadSome(p.b.get(), &out, 1s).ok());
  EXPECT_EQ(out, "hello world");
}

TEST(SocketIo, ReadSomeTimesOutWithoutData) {
  Pair p;
  std::string out;
  const auto start = std::chrono::steady_clock::now();
  const Status st = ReadSome(p.b.get(), &out, 50ms);
  EXPECT_EQ(st.code(), StatusCode::kTimedOut);
  EXPECT_GE(std::chrono::steady_clock::now() - start, 40ms);
  EXPECT_TRUE(out.empty());
  EXPECT_EQ(ReadSome(p.b.get(), &out, 0ms).code(), StatusCode::kTimedOut);  // a pure poll
}

TEST(SocketIo, ReadSomeReportsEofAsIoError) {
  Pair p;
  p.a = Fd();  // closes
  std::string out;
  EXPECT_EQ(ReadSome(p.b.get(), &out, 1s).code(), StatusCode::kIOError);
}

TEST(SocketIo, WriteAllToAClosedPeerFailsWithoutSigpipe) {
  Pair p;
  p.b = Fd();
  const std::string chunk(size_t{1} << 16, 'y');
  Status st;
  for (int i = 0; i < 10 && st.ok(); ++i) st = WriteAll(p.a.get(), chunk, 1s);
  EXPECT_EQ(st.code(), StatusCode::kIOError);
}

TEST(SocketIo, WriteAllTimesOutWhenThePeerDoesNotRead) {
  Pair p;
  const std::string big(size_t{8} << 20, 'z');
  EXPECT_EQ(WriteAll(p.a.get(), big, 100ms).code(), StatusCode::kTimedOut);
}

TEST(SocketIo, ConnectTcpToAClosedPortFails) {
  uint16_t port = 0;
  { Fd l = Listen(&port); }  // bound then closed: nobody listens there now
  auto fd = ConnectTcp("127.0.0.1", port, 1s);
  ASSERT_FALSE(fd.ok());
  EXPECT_EQ(fd.status().code(), StatusCode::kIOError);
}

TEST(SocketIo, ConnectTcpToAListeningPortSucceeds) {
  uint16_t port = 0;
  Fd l = Listen(&port);
  auto fd = ConnectTcp("127.0.0.1", port, 1s);
  ASSERT_TRUE(fd.ok()) << fd.status().ToString();
  EXPECT_GE(fd->get(), 0);
  Fd accepted(::accept(l.get(), nullptr, nullptr));
  ASSERT_GE(accepted.get(), 0);
  ASSERT_TRUE(WriteAll(fd->get(), "ping", 1s).ok());
  std::string out;
  ASSERT_TRUE(ReadSome(accepted.get(), &out, 1s).ok());
  EXPECT_EQ(out, "ping");
}

// A full accept queue makes the kernel drop further SYNs, so a connect hangs until it is cancelled.
TEST(SocketIo, ConnectTcpGivesUpPromptlyWhenCancelled) {
  uint16_t port = 0;
  Fd l(::socket(AF_INET, SOCK_STREAM, 0));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(::bind(l.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
  ASSERT_EQ(::listen(l.get(), 0), 0);
  socklen_t len = sizeof(addr);
  ASSERT_EQ(::getsockname(l.get(), reinterpret_cast<sockaddr*>(&addr), &len), 0);
  port = ntohs(addr.sin_port);
  std::vector<Fd> fillers;
  for (int i = 0; i < 4; ++i) {
    auto f = ConnectTcp("127.0.0.1", port, 100ms);
    if (f.ok()) fillers.push_back(std::move(f).value());
  }
  std::atomic<bool> cancel{false};
  std::thread canceller([&] {
    std::this_thread::sleep_for(200ms);
    cancel = true;
  });
  const auto start = std::chrono::steady_clock::now();
  auto fd = ConnectTcp("127.0.0.1", port, 10s, &cancel);
  const auto took = std::chrono::steady_clock::now() - start;
  canceller.join();
  if (fd.ok()) GTEST_SKIP() << "this kernel completed the connect despite the full accept queue";
  EXPECT_EQ(fd.status().code(), StatusCode::kIOError);
  EXPECT_EQ(fd.status().message(), "connect: cancelled");
  EXPECT_GE(took, 150ms);
  EXPECT_LT(took, 1s);
}

TEST(SocketIo, ConnectTcpRejectsAnUnresolvableHost) {
  auto fd = ConnectTcp("no-such-host.invalid", 6379, 500ms);
  ASSERT_FALSE(fd.ok());
  EXPECT_EQ(fd.status().code(), StatusCode::kIOError);
}

TEST(SocketIo, RespCommandEncodesAnArrayOfBulks) {
  EXPECT_EQ(RespCommand({"REPLCONF", "ACK", "12"}), "*3\r\n$8\r\nREPLCONF\r\n$3\r\nACK\r\n$2\r\n12\r\n");
  EXPECT_EQ(RespCommand({}), "*0\r\n");
  EXPECT_EQ(RespCommand({std::string_view("a\0b", 3)}), std::string("*1\r\n$3\r\na\0b\r\n", 13));
}

}  // namespace
}  // namespace tideskv::repl
