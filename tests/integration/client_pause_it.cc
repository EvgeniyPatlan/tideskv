#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "support/it_helpers.h"
#include "support/test_server.h"

namespace tideskv::test {
namespace {

using namespace std::chrono_literals;

TEST(ClientPauseIT, WriteModeHoldsWritesUntilUnpauseAndReadsGoOn) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto admin = s.Connect();
  auto writer = s.Connect();
  auto reader = s.Connect();
  ASSERT_EQ(admin->Command({"CLIENT", "PAUSE", "30000", "WRITE"}).str, "OK");
  ASSERT_TRUE(writer->Send({"SET", "k", "v"}));
  ASSERT_TRUE(WaitFor([&] { return InfoField(*reader, "clients", "blocked_clients") == 1; }));
  EXPECT_EQ(reader->Command({"GET", "k"}).type, RespValue::Type::kNull);
  EXPECT_EQ(reader->Command({"PING"}).str, "PONG");
  ASSERT_EQ(admin->Command({"CLIENT", "UNPAUSE"}).str, "OK");
  const auto r = writer->Read();
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->str, "OK");
  EXPECT_EQ(reader->Command({"GET", "k"}).str, "v");
  EXPECT_EQ(InfoField(*reader, "clients", "blocked_clients"), 0);
}

TEST(ClientPauseIT, ThePauseEndsByItselfAndResumesAPipeline) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto admin = s.Connect();
  auto c = s.Connect();
  ASSERT_EQ(admin->Command({"CLIENT", "PAUSE", "300", "ALL"}).str, "OK");
  ASSERT_TRUE(c->SendRaw(EncodeCommand({"SET", "a", "1"}) + EncodeCommand({"GET", "a"})));
  const auto first = c->Read();
  const auto second = c->Read();
  ASSERT_TRUE(first.has_value() && second.has_value());
  EXPECT_EQ(first->str, "OK");
  EXPECT_EQ(second->str, "1");
}

// D21: expiry pauses with the clients, so the master's sequence holds still while a controller compares
// replica offsets.
TEST(ClientPauseIT, TheReaperIsPausedToo) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  for (int i = 0; i < 50; ++i)
    ASSERT_EQ(c->Command({"SET", "e" + std::to_string(i), "v", "PX", "1000"}).str, "OK");
  ASSERT_EQ(c->Command({"CLIENT", "PAUSE", "3000", "WRITE"}).str, "OK");
  ASSERT_EQ(InfoField(*c, "stats", "expired_keys"), 0);  // nothing was reaped before the pause began
  const int64_t seq = InfoField(*c, "keyspace", "sequence");
  std::this_thread::sleep_for(1500ms);  // the keys fall due during the pause; an unpaused reaper deletes them
  EXPECT_EQ(InfoField(*c, "keyspace", "sequence"), seq);
  EXPECT_EQ(InfoField(*c, "stats", "expired_keys"), 0);
  EXPECT_TRUE(WaitFor([&] { return InfoField(*c, "keyspace", "sequence") > seq; }, 5s));
}

}  // namespace
}  // namespace tideskv::test
