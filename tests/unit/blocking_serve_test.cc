#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include "commands/blocking_serve.h"
#include "commands/cmd_util.h"
#include "script/script_engine.h"
#include "support/command_harness.h"
#include "types/errors.h"
#include "types/list_move.h"
#include "types/list_type.h"

namespace tideskv {
namespace {

using test::Bulk;
using test::CommandHarness;
using test::Err;
using test::Int;
using test::SecondClient;

// Set by a test to make the next TBPOP act as if every key were empty (simulates a push landing between
// the pop attempt and the registration).
bool g_pretend_empty = false;
bool g_rerun = false;  // TTHROW: the next run is a re-run

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

// TTHROW key: blocks on key the first time it runs; every re-run throws (a command bug on the serve path).
void TThrow(ExecContext& ctx, const ArgList& argv) {
  if (!std::exchange(g_rerun, true)) {
    return ctx.block->Block({encoding::ValueType::kList, {std::string(argv[1])}, 0});
  }
  throw std::runtime_error("tthrow re-run");
}

struct BlockingServe : ::testing::Test {
  void SetUp() override {
    g_pretend_empty = false;
    g_rerun = false;
    h.services.registry_.Register({"tbpop", -2, kCmdWrite, 1, -1, 1, &TBPop});
    h.services.registry_.Register({"tpush", -3, kCmdWrite, 1, 1, 1, &TPush});
    h.services.registry_.Register({"tthrow", 2, kCmdWrite, 1, 1, 1, &TThrow});
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

TEST_F(BlockingServe, WrongTypeWaiterIsSkippedAndTheNextOneServed) {
  EXPECT_EQ(h.Run({"BZPOPMIN", "k", "0"}), "");  // waits for a zset
  SecondClient b(h), pusher(h);
  EXPECT_EQ(b.Run({"TBPOP", "k"}), "");  // waits for a list, behind the zset waiter
  EXPECT_EQ(pusher.Run({"TPUSH", "k", "x"}), Int(1));
  EXPECT_EQ(b.TakeUnblocked(), (std::vector<std::string>{Bulk("x")}));
  EXPECT_TRUE(h.TakeUnblocked().empty());
  EXPECT_EQ(h.services.blocking_.blocked_clients(), 1u);  // the zset waiter still waits
}

TEST_F(BlockingServe, ServedCommandCascadesToItsDestinationsWaiters) {
  EXPECT_EQ(h.Run({"BLMOVE", "src", "dst", "LEFT", "RIGHT", "0"}), "");
  SecondClient b(h), pusher(h);
  EXPECT_EQ(b.Run({"TBPOP", "dst"}), "");
  EXPECT_EQ(pusher.Run({"TPUSH", "src", "v"}), Int(1));
  EXPECT_EQ(h.TakeUnblocked(), (std::vector<std::string>{Bulk("v")}));  // moved src -> dst
  EXPECT_EQ(b.TakeUnblocked(), (std::vector<std::string>{Bulk("v")}));  // dst became ready in the same call
  EXPECT_EQ(pusher.Run({"EXISTS", "src", "dst"}), Int(0));
  EXPECT_EQ(h.services.blocking_.blocked_clients(), 0u);
}

TEST_F(BlockingServe, ReRunThatThrowsEndsThatWaitWithAnErrorNotThePusher) {
  EXPECT_EQ(h.Run({"TTHROW", "k"}), "");
  SecondClient b(h), pusher(h);
  EXPECT_EQ(b.Run({"TBPOP", "k"}), "");
  EXPECT_EQ(pusher.Run({"TPUSH", "k", "x"}), Int(1));  // the exception did not reach the pusher
  EXPECT_EQ(h.TakeUnblocked(), (std::vector<std::string>{Err(kErrInternal)}));
  EXPECT_EQ(b.TakeUnblocked(), (std::vector<std::string>{Bulk("x")}));  // the next waiter is served
  EXPECT_EQ(h.services.blocking_.blocked_clients(), 0u);
}

// Stands in for a script that holds the gate (its batch) and has passed busy-reply-threshold, then ends
// by itself after `hold`. Its end clears BUSY before the gate is released, as RunScript does.
class TimedOutScript {
 public:
  TimedOutScript(ServerServices& server, std::chrono::milliseconds hold) {
    thread_ = std::thread([&server, hold, this] {
      auto batch = server.engine().BeginBatch();
      started_ = batch.ok() ? 1 : 2;
      std::this_thread::sleep_for(hold);
      server.scripts().ForceTimedOutForTests(false);
    });
    while (started_.load() == 0) std::this_thread::yield();
    server.scripts().ForceTimedOutForTests(true);
  }
  ~TimedOutScript() { Join(); }
  TimedOutScript(const TimedOutScript&) = delete;
  TimedOutScript& operator=(const TimedOutScript&) = delete;
  bool started() const { return started_.load() == 1; }
  void Join() {
    if (thread_.joinable()) thread_.join();
  }

 private:
  std::atomic<int> started_{0};
  std::thread thread_;
};

struct BlockingServeBusy : BlockingServe {
  void SetUp() override {
    BlockingServe::SetUp();
    ScriptEngine& scripts = h.services.scripts_;
    h.services.engine_.SetBusyGuard([&scripts] { return scripts.TimedOut(); }, std::string(kErrBusyScript));
  }
};

// A push committed just before a script took the gate; its serve runs while the script is timed out. The
// serve waits for the script (as Redis serves blocked clients after it) instead of giving up with BUSY and
// stranding the waiter with data present.
TEST_F(BlockingServeBusy, ServeDuringATimedOutScriptWaitsForItThenServes) {
  g_pretend_empty = true;
  EXPECT_EQ(h.Run({"TBPOP", "k"}), "");
  ASSERT_EQ(h.services.blocking_.blocked_clients(), 1u);
  ASSERT_TRUE(
      h.services.engine_
          .Write(
              0, {"k"},
              [&](WriteTxn& txn, int64_t now) {
                return ListPush(txn, h.services.oids(), 0, "k", ListEnd::kRight, {"a"}, false, now).status();
              })
          .ok());
  std::atomic<bool> served{false};
  {
    TimedOutScript script(h.services, std::chrono::milliseconds(200));
    ASSERT_TRUE(script.started());
    std::thread pusher([&] {  // the pusher's serve, during BUSY
      ReadyKeys ready{{0, "k"}};
      ServeBlockedClients(h.services.registry_, h.services, ready);
      served = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(served.load()) << "the serve gave up instead of waiting for the script";
    script.Join();
    pusher.join();
  }
  EXPECT_EQ(h.TakeUnblocked(), (std::vector<std::string>{Bulk("a")}));
  EXPECT_EQ(h.Run({"LLEN", "k"}), Int(0));
  EXPECT_EQ(h.services.blocking_.blocked_clients(), 0u);
}

std::unique_ptr<TimedOutScript> g_script;
// First run: blocks. Re-run: a script takes the gate and times out just before the pop, in the window
// between the serve's type check and the re-run's own Engine call.
void TBusyPop(ExecContext& ctx, const ArgList& argv) {
  if (!std::exchange(g_rerun, true)) {
    return ctx.block->Block({encoding::ValueType::kList, {std::string(argv[1])}, 0});
  }
  g_script = std::make_unique<TimedOutScript>(dynamic_cast<ServerServices&>(ctx.server),
                                              std::chrono::milliseconds(100));
  TBPop(ctx, argv);
}

TEST_F(BlockingServeBusy, BlockedClientNeverReceivesBusy) {
  h.services.registry_.Register({"tbusypop", 2, kCmdWrite, 1, 1, 1, &TBusyPop});
  EXPECT_EQ(h.Run({"TBUSYPOP", "k"}), "");
  SecondClient other(h);
  EXPECT_EQ(other.Run({"RPUSH", "k", "a"}), Int(1));
  ASSERT_NE(g_script, nullptr);
  g_script.reset();  // joins
  EXPECT_EQ(h.TakeUnblocked(), (std::vector<std::string>{Bulk("a")}));
  EXPECT_EQ(h.Run({"LLEN", "k"}), Int(0));
  EXPECT_EQ(h.services.blocking_.blocked_clients(), 0u);
}

}  // namespace
}  // namespace tideskv
