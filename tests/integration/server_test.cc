#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "support/test_server.h"

// Designated initializers deliberately leave most TestServer::Options fields defaulted.
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace tideskv::test {
namespace {

using Type = RespValue::Type;

TEST(ServerIT, PingEchoAndBinarySafety) {  // Review Focus #3
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"PING"}).str, "PONG");
  const std::string bin("\0\r\n\xff", 4);
  EXPECT_EQ(c->Command({"ECHO", bin}).str, bin);
}

TEST(ServerIT, InlineAndPipelinedRequests) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->SendRaw("PING\r\nPING\r\n"));
  EXPECT_EQ(c->Read()->str, "PONG");
  EXPECT_EQ(c->Read()->str, "PONG");
  std::string batch;
  for (int i = 0; i < 50; ++i) batch += EncodeCommand({"ECHO", std::to_string(i)});
  ASSERT_TRUE(c->SendRaw(batch));
  for (int i = 0; i < 50; ++i) EXPECT_EQ(c->Read()->str, std::to_string(i));
}

TEST(ServerIT, Hello3ReturnsMap) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  const RespValue r = c->Command({"HELLO", "3"});
  ASSERT_EQ(r.type, Type::kMap) << ToDebugString(r);
  EXPECT_EQ(r.elements[0].str, "server");
  EXPECT_EQ(r.elements[1].str, "redis");
}

TEST(ServerIT, RequirePassFlow) {
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--requirepass", "s3cret"}});
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"PING"}).str, "NOAUTH Authentication required.");
  EXPECT_EQ(c->Command({"AUTH", "nope"}).type, Type::kError);
  EXPECT_EQ(c->Command({"AUTH", "s3cret"}).str, "OK");
  EXPECT_EQ(c->Command({"PING"}).str, "PONG");
}

TEST(ServerIT, ConfigSetRequirepassAffectsNewConnectionsOnly) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto c1 = s.Connect();
  ASSERT_NE(c1, nullptr);
  EXPECT_EQ(c1->Command({"CONFIG", "SET", "requirepass", "x"}).str, "OK");
  EXPECT_EQ(c1->Command({"PING"}).str, "PONG");
  auto c2 = s.Connect();
  ASSERT_NE(c2, nullptr);
  EXPECT_EQ(c2->Command({"PING"}).str, "NOAUTH Authentication required.");
}

TEST(ServerIT, UnknownCommandAndProtocolError) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"NOPE"}).str, "ERR unknown command 'NOPE', with args beginning with: ");
  ASSERT_TRUE(c->SendRaw("*1\r\n$abc\r\n"));
  EXPECT_EQ(c->Read()->str, "ERR Protocol error: invalid bulk length");
  EXPECT_TRUE(c->WaitForClose());
}

TEST(ServerIT, InfoReportsPortAndClients) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  // WaitReady's probe connection is released asynchronously; poll until it is gone.
  std::string info;
  for (int i = 0; i < 100; ++i) {
    info = c->Command({"INFO"}).str;
    if (info.find("connected_clients:1") != std::string::npos) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_NE(info.find("tcp_port:" + std::to_string(s.port())), std::string::npos) << info;
  EXPECT_NE(info.find("connected_clients:1"), std::string::npos) << info;
  EXPECT_NE(info.find("storage_engine:tidesdb"), std::string::npos) << info;
}

TEST(ServerIT, ShutdownCommandExitsZero) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(c->Send({"SHUTDOWN"}));
  EXPECT_TRUE(c->WaitForClose());
  EXPECT_EQ(s.Wait(), 0) << s.Stderr();
}

TEST(ServerIT, SigtermExitsZeroWithClientsConnected) {  // Review Focus #4
  TestServer s(TIDESKV_SERVER_BIN);
  auto idle = s.Connect();
  auto partial = s.Connect();
  ASSERT_NE(idle, nullptr);
  ASSERT_NE(partial, nullptr);
  ASSERT_TRUE(partial->SendRaw("*2\r\n$4\r\nECHO\r\n"));
  EXPECT_EQ(s.Terminate(), 0) << s.Stderr();
}

TEST(ServerIT, AbruptClientDisconnectDoesNotAffectServer) {  // Review Focus #4
  TestServer s(TIDESKV_SERVER_BIN);
  {
    auto c = s.Connect();
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(c->SendRaw("*2\r\n$4\r\nECHO\r\n$100\r\nabc"));
  }
  auto c2 = s.Connect();
  ASSERT_NE(c2, nullptr);
  EXPECT_EQ(c2->Command({"PING"}).str, "PONG");
}

TEST(ServerIT, SecondInstanceOnSameDirExitsWithCode2) {  // Review Focus #5
  TestServer first(TIDESKV_SERVER_BIN);
  TestServer second(TIDESKV_SERVER_BIN, {.dir = first.dir(), .wait_ready = false});
  EXPECT_EQ(second.Wait(), 2);
  EXPECT_NE(second.Stderr().find("locked"), std::string::npos) << second.Stderr();
  auto c = first.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"PING"}).str, "PONG");
}

TEST(ServerIT, RestartReusesDataDirectory) {  // Review Focus #5
  TempDir dir("tideskv-it-restart");
  {
    TestServer a(TIDESKV_SERVER_BIN, {.dir = dir.str()});
    EXPECT_EQ(a.Terminate(), 0) << a.Stderr();
  }
  TestServer b(TIDESKV_SERVER_BIN, {.dir = dir.str()});
  auto c = b.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"PING"}).str, "PONG");
}

TEST(ServerIT, PortInUseExitsWithCode3) {
  TestServer first(TIDESKV_SERVER_BIN);
  TestServer second(TIDESKV_SERVER_BIN, {.port = first.port(), .wait_ready = false});
  EXPECT_EQ(second.Wait(), 3) << second.Stderr();
}

TEST(ServerIT, BadConfigExitsWithCode1) {
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--port", "0"}, .wait_ready = false});
  EXPECT_EQ(s.Wait(), 1) << s.Stderr();
}

TEST(ServerIT, RequirepassFromEnvironment) {
  TestServer s(TIDESKV_SERVER_BIN, {.env = {{"TIDESKV_REQUIREPASS", "envpw"}}});
  auto c = s.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"PING"}).str, "NOAUTH Authentication required.");
  EXPECT_EQ(c->Command({"AUTH", "envpw"}).str, "OK");
  EXPECT_EQ(s.Stderr().find("envpw"), std::string::npos);  // never logged
}

}  // namespace
}  // namespace tideskv::test
