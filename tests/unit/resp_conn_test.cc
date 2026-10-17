#include "repl/resp_conn.h"

#include <gtest/gtest.h>
#include <sys/socket.h>

#include <chrono>
#include <string>
#include <thread>

#include "repl/repl_errors.h"
#include "repl/socket_io.h"

namespace tideskv::repl {
namespace {

using namespace std::chrono_literals;

struct Pair {
  Pair() {
    int fds[2] = {-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    near = Fd(fds[0]);
    far = Fd(fds[1]);
  }
  Fd near;
  Fd far;
};

TEST(RespConn, CommandWritesTheRequestAndReadsOneLine) {
  Pair p;
  std::string in;
  RespConn conn(p.near.get(), &in, 1s);
  std::thread peer([&] {
    std::string got;
    while (got.find("PING\r\n") == std::string::npos) ASSERT_TRUE(ReadSome(p.far.get(), &got, 1s).ok());
    EXPECT_EQ(got, "*1\r\n$4\r\nPING\r\n");
    ASSERT_TRUE(WriteAll(p.far.get(), "+PONG\r\n*1\r\n$4\r\nNEXT\r\n", 1s).ok());
  });
  auto reply = conn.Command({"PING"});
  peer.join();
  ASSERT_TRUE(reply.ok()) << reply.status().ToString();
  EXPECT_EQ(reply->type, resp::ReplyNode::Type::kStatus);
  EXPECT_EQ(reply->str, "PONG");
  // What followed the reply line stays for the owner (ReplClient's stream after +CONTINUE).
  for (int i = 0; i < 100 && in.find("NEXT") == std::string::npos; ++i)
    (void)ReadSome(p.near.get(), &in, 10ms);
  EXPECT_NE(in.find("*1\r\n$4\r\nNEXT\r\n"), std::string::npos);
}

TEST(RespConn, AnErrorReplyIsAValueNotAFailure) {
  Pair p;
  std::string in;
  RespConn conn(p.near.get(), &in, 1s);
  ASSERT_TRUE(WriteAll(p.far.get(), "-NOAUTH Authentication required.\r\n", 1s).ok());
  auto reply = conn.Command({"CLUSTER", "IMPORT", "1", "0"});
  ASSERT_TRUE(reply.ok());
  EXPECT_EQ(reply->type, resp::ReplyNode::Type::kError);
  EXPECT_EQ(reply->str, "NOAUTH Authentication required.");
}

TEST(RespConn, TimesOutWithoutAReplyAndFailsOnEof) {
  Pair p;
  std::string in;
  RespConn conn(p.near.get(), &in, 50ms);
  auto timed_out = conn.Command({"PING"});
  EXPECT_EQ(timed_out.status().code(), StatusCode::kTimedOut);
  EXPECT_EQ(timed_out.status().message(), kMsgNoReplyWithinTimeout);
  p.far = Fd();  // the peer closes
  EXPECT_EQ(conn.ReadLine().status().code(), StatusCode::kIOError);
}

TEST(RespConn, AnUnreadableReplyIsAProtocolError) {
  Pair p;
  std::string in;
  RespConn conn(p.near.get(), &in, 1s);
  ASSERT_TRUE(WriteAll(p.far.get(), "?what\r\n", 1s).ok());
  auto reply = conn.Command({"PING"});
  EXPECT_EQ(reply.status().code(), StatusCode::kIOError);
  EXPECT_EQ(reply.status().message(), "protocol error: unreadable reply to PING");
}

}  // namespace
}  // namespace tideskv::repl
