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

// A client that disconnects while parked leaves the count; UNPAUSE still works (review fix 1, #3).
TEST(ClientPauseIT, AClientClosedWhileParkedIsForgotten) {
  TestServer s(TIDESKV_SERVER_BIN);
  auto admin = s.Connect();
  auto writer = s.Connect();
  ASSERT_EQ(admin->Command({"CLIENT", "PAUSE", "30000", "WRITE"}).str, "OK");
  ASSERT_TRUE(writer->Send({"SET", "k", "v"}));
  ASSERT_TRUE(WaitFor([&] { return InfoField(*admin, "clients", "blocked_clients") == 1; }));
  writer.reset();
  EXPECT_TRUE(WaitFor([&] { return InfoField(*admin, "clients", "blocked_clients") == 0; }));
  ASSERT_EQ(admin->Command({"CLIENT", "UNPAUSE"}).str, "OK");
  EXPECT_EQ(admin->Command({"GET", "k"}).type, RespValue::Type::kNull);  // the parked write never ran
  EXPECT_EQ(admin->Command({"SET", "k", "w"}).str, "OK");
}

// D21: expiry pauses with the clients, so the master's sequence holds still while a controller compares
// replica offsets. Robust under load: the keys live 3 s and the pause lasts until UNPAUSE; if setting them up
// took so long that the pause may have begun after they fell due, the test skips instead of failing.
TEST(ClientPauseIT, TheReaperIsPausedToo) {
  constexpr auto kTtl = 3000ms;
  constexpr auto kMaxSetup = 2000ms;  // the pause must begin with at least 1 s of TTL left
  TestServer s(TIDESKV_SERVER_BIN);
  auto c = s.Connect();
  const auto start = std::chrono::steady_clock::now();  // every key's deadline is after start + kTtl
  for (int i = 0; i < 50; ++i) {
    ASSERT_EQ(c->Command({"SET", "e" + std::to_string(i), "v", "PX", std::to_string(kTtl.count())}).str,
              "OK");
  }
  ASSERT_EQ(c->Command({"CLIENT", "PAUSE", "60000", "WRITE"}).str, "OK");
  if (std::chrono::steady_clock::now() - start > kMaxSetup) {
    GTEST_SKIP() << "setup took over 2 s (loaded host): the pause may have begun after the keys fell due";
  }
  ASSERT_EQ(InfoField(*c, "stats", "expired_keys"), 0);
  const int64_t seq = InfoField(*c, "keyspace", "sequence");
  std::this_thread::sleep_until(start + kTtl + 1000ms);  // every key is due; an unpaused reaper deletes them
  EXPECT_EQ(InfoField(*c, "keyspace", "sequence"), seq);
  EXPECT_EQ(InfoField(*c, "stats", "expired_keys"), 0);
  ASSERT_EQ(c->Command({"CLIENT", "UNPAUSE"}).str, "OK");
  EXPECT_TRUE(WaitFor([&] { return InfoField(*c, "stats", "expired_keys") == 50; }, 10s));
  EXPECT_GT(InfoField(*c, "keyspace", "sequence"), seq);
}

}  // namespace
}  // namespace tideskv::test
