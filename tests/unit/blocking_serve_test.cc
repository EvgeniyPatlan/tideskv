#include <gtest/gtest.h>

#include "commands/blocking_serve.h"
#include "commands/cmd_util.h"
#include "support/command_harness.h"
#include "types/list_move.h"

namespace tideskv {
namespace {

using test::Arr;
using test::Bulk;
using test::CommandHarness;
using test::Int;
using test::SecondClient;

// Set by a test to make the next TBPOP act as if every key were empty (simulates a push landing between
// the pop attempt and the registration).
bool g_pretend_empty = false;

// TBPOP key [key ...]: BLPOP with an infinite timeout and a plain bulk reply; TPUSH key v: RPUSH that
// signals.
void TBPop(ExecContext& ctx, const ArgList& argv) {
  if (std::exchange(g_pretend_empty, false) && ctx.block != nullptr) {
    return ctx.block->Block(
        {encoding::ValueType::kList, std::vector<std::string>(argv.begin() + 1, argv.end()), 0});
  }
  const auto keys = ArgsFrom(argv, 1);
  std::optional<PoppedFrom> popped;
  const Status st = ctx.server.engine().Write(ctx.session.db, keys, [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(ListPopFirst(txn, ctx.session.db, keys, ListEnd::kLeft, 1, now_ms), &popped);
  });
  if (!st.ok()) return ReplyError(ctx, st);
  if (popped) return ctx.reply.Bulk(popped->values.front());
  if (ctx.block == nullptr) return ctx.reply.NullArray();
  ctx.block->Block({encoding::ValueType::kList, std::vector<std::string>(argv.begin() + 1, argv.end()), 0});
}

void TPush(ExecContext& ctx, const ArgList& argv) {
  int64_t n = 0;
  const bool ok = WriteOneKey(ctx, argv[1], [&](WriteTxn& txn, int64_t now_ms) {
    return MoveInto(ListPush(txn, ctx.server.oids(), ctx.session.db, argv[1], ListEnd::kRight,
                             ArgsFrom(argv, 2), false, now_ms),
                    &n);
  });
  if (!ok) return;
  SignalReady(ctx, argv[1]);
  ctx.reply.Integer(n);
}

struct BlockingServe : ::testing::Test {
  void SetUp() override {
    g_pretend_empty = false;
    h.services.registry_.Register({"tbpop", -2, kCmdWrite, 1, -1, 1, &TBPop});
    h.services.registry_.Register({"tpush", -3, kCmdWrite, 1, 1, 1, &TPush});
  }
  CommandHarness h;
};

TEST_F(BlockingServe, PushServesTheWaiterBeforeItsOwnReply) {
  EXPECT_EQ(h.Run({"TBPOP", "k"}), "");  // blocked: no reply yet
  ASSERT_NE(h.session.blocked, nullptr);
  EXPECT_EQ(h.services.blocking_.blocked_clients(), 1u);
  SecondClient other(h);
  EXPECT_EQ(other.Run({"TPUSH", "k", "a", "b"}), Int(2));  // counts before the waiter pops
  EXPECT_EQ(h.TakeUnblocked(), (std::vector<std::string>{Bulk("a")}));
  EXPECT_EQ(other.Run({"LLEN", "k"}), Int(1));  // already popped when TPUSH replied
  EXPECT_EQ(h.services.blocking_.blocked_clients(), 0u);
}

TEST_F(BlockingServe, FifoOneElementPerWaiter) {
  SecondClient b(h), c(h);
  b.session.id = 20;
  c.session.id = 21;
  EXPECT_EQ(h.Run({"TBPOP", "k"}), "");
  EXPECT_EQ(b.Run({"TBPOP", "k"}), "");
  EXPECT_EQ(c.Run({"TPUSH", "k", "1"}), Int(1));
  EXPECT_EQ(h.TakeUnblocked(), (std::vector<std::string>{Bulk("1")}));
  EXPECT_TRUE(b.TakeUnblocked().empty());  // still waiting
  EXPECT_EQ(c.Run({"TPUSH", "k", "2"}), Int(1));
  EXPECT_EQ(b.TakeUnblocked(), (std::vector<std::string>{Bulk("2")}));
}

TEST_F(BlockingServe, PushBetweenCheckAndRegistrationIsNotLost) {
  // The element is already there, but TBPOP behaves as if its pop attempt had run just before it arrived.
  // The self-signal in ConnectionBlockHandler makes the serve loop that follows find it.
  ASSERT_EQ(h.Run({"RPUSH", "k", "late"}), Int(1));  // plain RPUSH: no waiters, no signal
  g_pretend_empty = true;
  EXPECT_EQ(h.Run({"TBPOP", "k"}), "");
  EXPECT_EQ(h.TakeUnblocked(), (std::vector<std::string>{Bulk("late")}));
  EXPECT_EQ(h.services.blocking_.blocked_clients(), 0u);
}

TEST_F(BlockingServe, WaiterOfAnotherTypeIsSkipped) {
  EXPECT_EQ(h.Run({"TBPOP", "k"}), "");
  SecondClient other(h);
  EXPECT_EQ(other.Run({"SADD", "k", "m"}), Int(1));
  ReadyKeys ready{{0, "k"}};
  ServeBlockedClients(h.services.registry(), h.services, ready);  // as if SADD had signalled
  EXPECT_TRUE(h.TakeUnblocked().empty());
  EXPECT_EQ(h.services.blocking_.blocked_clients(), 1u);
}

}  // namespace
}  // namespace tideskv
