#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "support/it_helpers.h"
#include "support/test_server.h"
#include "types/errors.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace tideskv::test {
namespace {

using Type = RespValue::Type;
using namespace std::chrono_literals;

struct BlockingOrderIT : ::testing::Test {
  TestServer s{TIDESKV_SERVER_BIN, {.extra_args = {"--workers", "4"}}};
  std::unique_ptr<RespClient> Client() {
    auto c = s.Connect();
    EXPECT_NE(c, nullptr);
    return c;
  }
  void WaitBlocked(RespClient& probe, int64_t n) {
    ASSERT_TRUE(WaitFor([&] { return InfoField(probe, "clients", "blocked_clients") == n; }));
  }
};

TEST_F(BlockingOrderIT, PusherSeesTheWaitersPopBeforeItsNextCommand) {  // Review Focus #2
  auto a = Client(), b = Client();
  ASSERT_TRUE(a->Send({"BLPOP", "k", "0"}));
  WaitBlocked(*b, 1);
  EXPECT_EQ(b->Command({"LPUSH", "k", "v"}).integer, 1);
  EXPECT_EQ(b->Command({"LLEN", "k"}).integer, 0);  // already popped when LPUSH replied
  EXPECT_EQ(b->Command({"EXISTS", "k"}).integer, 0);
  auto r = a->Read();
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->elements.size(), 2u);
  EXPECT_EQ(r->elements[0].str, "k");
  EXPECT_EQ(r->elements[1].str, "v");
  EXPECT_EQ(InfoField(*b, "clients", "blocked_clients"), 0);
}

TEST_F(BlockingOrderIT, TimeoutsReplyNullThenRunThePipeline) {  // Review Focus #4
  auto a = Client();
  const auto start = std::chrono::steady_clock::now();
  ASSERT_TRUE(
      a->SendRaw(EncodeCommand({"BLMOVE", "src", "dst", "LEFT", "LEFT", "0.2"}) + EncodeCommand({"PING"})));
  auto r1 = a->Read();
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1->type, Type::kNull);  // *-1 on a real timeout, even for BLMOVE
  EXPECT_GE(std::chrono::steady_clock::now() - start, 190ms);
  auto r2 = a->Read();
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2->str, "PONG");
  EXPECT_EQ(a->Command({"HELLO", "3"}).type, Type::kMap);
  ASSERT_TRUE(a->SendRaw(EncodeCommand({"BZPOPMIN", "z", "0.05"})));
  auto r3 = a->Read();
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(r3->type, Type::kNull);  // RESP3 "_"
}

TEST_F(BlockingOrderIT, PipelinedBlpopsAreServedOneAfterTheOther) {  // Review Focus #2
  auto a = Client(), b = Client();
  ASSERT_TRUE(a->SendRaw(EncodeCommand({"BLPOP", "q", "0"}) + EncodeCommand({"BLPOP", "q", "0"})));
  WaitBlocked(*b, 1);
  EXPECT_EQ(b->Command({"RPUSH", "q", "1"}).integer, 1);
  auto r1 = a->Read();
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1->elements[1].str, "1");
  WaitBlocked(*b, 1);  // the second BLPOP only now waits
  EXPECT_EQ(b->Command({"RPUSH", "q", "2"}).integer, 1);
  auto r2 = a->Read();
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2->elements[1].str, "2");
}

TEST_F(BlockingOrderIT, DisconnectedWaiterConsumesNothing) {  // Review Focus #4
  auto a = Client(), b = Client();
  ASSERT_TRUE(a->Send({"BLPOP", "k", "0"}));
  WaitBlocked(*b, 1);
  a->Close();
  WaitBlocked(*b, 0);
  EXPECT_EQ(b->Command({"RPUSH", "k", "v"}).integer, 1);
  EXPECT_EQ(b->Command({"LLEN", "k"}).integer, 1);
}

TEST_F(BlockingOrderIT, ClientUnblockAcrossConnections) {
  auto a = Client(), b = Client();
  const int64_t id = a->Command({"CLIENT", "ID"}).integer;
  ASSERT_TRUE(a->Send({"BLPOP", "k", "0"}));
  WaitBlocked(*b, 1);
  EXPECT_EQ(b->Command({"CLIENT", "UNBLOCK", std::to_string(id), "ERROR"}).integer, 1);
  auto r = a->Read();
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->type, Type::kError);
  EXPECT_EQ(r->str, kErrUnblockedByClient);
  EXPECT_EQ(a->Command({"PING"}).str, "PONG");  // the connection works normally afterwards
}

TEST_F(BlockingOrderIT, FifoAcrossClientsAndBlmoveCascade) {
  auto a = Client(), b = Client(), c = Client(), p = Client();
  ASSERT_TRUE(a->Send({"BLMOVE", "src", "mid", "RIGHT", "LEFT", "0"}));
  WaitBlocked(*p, 1);
  ASSERT_TRUE(b->Send({"BLPOP", "mid", "0"}));
  WaitBlocked(*p, 2);
  ASSERT_TRUE(c->Send({"BLPOP", "src", "0"}));
  WaitBlocked(*p, 3);
  EXPECT_EQ(p->Command({"RPUSH", "src", "x", "y"}).integer, 2);
  auto ra = a->Read();
  ASSERT_TRUE(ra.has_value());
  EXPECT_EQ(ra->str, "y");  // first in line on src: BLMOVE takes the tail
  auto rc = c->Read();
  ASSERT_TRUE(rc.has_value());
  ASSERT_EQ(rc->elements.size(), 2u);
  EXPECT_EQ(rc->elements[1].str, "x");  // second in line gets the other element
  auto rb = b->Read();
  ASSERT_TRUE(rb.has_value());
  ASSERT_EQ(rb->elements.size(), 2u);
  EXPECT_EQ(rb->elements[1].str, "y");  // mid was created by the BLMOVE and served in the same pass
  EXPECT_EQ(p->Command({"EXISTS", "src", "mid"}).integer, 0);
}

}  // namespace
}  // namespace tideskv::test
