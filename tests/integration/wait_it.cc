// A port of Redis 7.2 unit/wait.tcl plus the Kvrocks wait cases (M6b D9).
#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "support/it_helpers.h"
#include "support/repl_pair.h"
#include "support/test_server.h"

namespace tideskv::test {
namespace {

using namespace std::chrono_literals;

struct WaitIT : ReplPairIT {};

TEST_F(WaitIT, AcknowledgesOneAdditionalCopy) {  // wait.tcl
  auto m = master.Connect();
  ASSERT_EQ(m->Command({"SET", "foo", "0"}).str, "OK");
  for (int i = 0; i < 3; ++i) m->Command({"INCR", "foo"});
  EXPECT_EQ(m->Command({"WAIT", "1", "5000"}).integer, 1);
  EXPECT_EQ(replica.Connect()->Command({"GET", "foo"}).str, "3");
}

TEST_F(WaitIT, DoesNotAcknowledgeTwoCopies) {  // wait.tcl
  auto m = master.Connect();
  m->Command({"INCR", "foo"});
  ASSERT_EQ(m->Command({"WAIT", "1", "5000"}).integer, 1);
  const auto t0 = std::chrono::steady_clock::now();
  const RespValue v = m->Command({"WAIT", "2", "1000"});  // times out with the count, not a null array
  EXPECT_GE(std::chrono::steady_clock::now() - t0, 900ms);
  ASSERT_EQ(v.type, RespValue::Type::kInteger);
  EXPECT_EQ(v.integer, 1);
}

TEST_F(WaitIT, DoesNotAcknowledgeWhileTheReplicaIsFrozen) {  // wait.tcl: pause_process
  auto m = master.Connect();
  replica.Pause();
  ASSERT_EQ(m->Command({"SET", "foo", "0"}).str, "OK");
  for (int i = 0; i < 3; ++i) m->Command({"INCR", "foo"});
  const RespValue frozen = m->Command({"WAIT", "1", "1000"});
  ASSERT_EQ(frozen.type, RespValue::Type::kInteger);
  EXPECT_EQ(frozen.integer, 0);
  replica.Resume();
  EXPECT_EQ(m->Command({"WAIT", "1", "4000"}).integer, 1);
}

TEST_F(WaitIT, TwoBlockedClientsAreReleasedTogether) {  // wait.tcl "multiple clients unblock"
  auto rd = master.Connect();
  auto rd2 = master.Connect();
  replica.Pause();
  rd->Command({"INCR", "foo"});
  rd2->Command({"INCR", "foo"});
  ASSERT_TRUE(rd->Send({"WAIT", "1", "0"}));
  ASSERT_TRUE(rd2->Send({"WAIT", "1", "0"}));
  auto m = master.Connect();
  ASSERT_TRUE(WaitFor([&] { return InfoField(*m, "clients", "blocked_clients") == 2; }));
  replica.Resume();
  EXPECT_EQ(rd->Read()->integer, 1);
  EXPECT_EQ(rd2->Read()->integer, 1);
  EXPECT_EQ(rd->Command({"PING"}).str, "PONG");
  EXPECT_EQ(InfoField(*m, "clients", "blocked_clients"), 0);
}

TEST_F(WaitIT, AServedBlockingPopIsTheClientsWrite) {  // Review Focus #5: Connection::OnUnblocked
  auto c = master.Connect();
  auto pusher = master.Connect();
  ASSERT_EQ(c->Command({"WAIT", "1", "5000"}).integer, 1);  // the replica is online
  replica.Pause();
  ASSERT_TRUE(c->Send({"BLPOP", "q", "0"}));
  ASSERT_TRUE(WaitFor([&] { return InfoField(*pusher, "clients", "blocked_clients") == 1; }));
  ASSERT_EQ(pusher->Command({"RPUSH", "q", "x"}).integer, 1);
  ASSERT_TRUE(c->Read().has_value());
  EXPECT_EQ(c->Command({"WAIT", "1", "300"}).integer, 0);  // the pop is not acknowledged yet
  replica.Resume();
  EXPECT_EQ(c->Command({"WAIT", "1", "4000"}).integer, 1);
}

TEST_F(WaitIT, BlocksUntilTheReplicaReturns) {  // Kvrocks TestWaitCommand
  auto r = replica.Connect();
  auto admin = master.Connect();
  ASSERT_EQ(r->Command({"REPLICAOF", "NO", "ONE"}).str, "OK");
  ASSERT_TRUE(WaitFor([&] { return InfoField(*admin, "replication", "connected_slaves") == 0; }, 10s));
  auto m = RespClient::Connect(master.port(), 30s);  // the full sync below may be slow under sanitizers
  ASSERT_EQ(m->Command({"SET", "k1", "v1"}).str, "OK");
  ASSERT_TRUE(m->Send({"WAIT", "1", "0"}));
  ASSERT_TRUE(WaitFor([&] { return InfoField(*admin, "clients", "blocked_clients") == 1; }));  // PF16
  ASSERT_EQ(r->Command({"REPLICAOF", "127.0.0.1", std::to_string(master.port())}).str, "OK");  // full sync
  auto reply = m->Read();
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(reply->integer, 1);
  EXPECT_EQ(r->Command({"GET", "k1"}).str, "v1");
}

TEST_F(WaitIT, AFullSyncingReplicaIsNotCounted) {  // Review Focus #5, D10
  auto m = master.Connect();
  auto r = replica.Connect();
  for (int i = 0; i < 5000; ++i) m->Command({"SET", "k" + std::to_string(i), "v"});
  ASSERT_EQ(r->Command({"REPLICAOF", "NO", "ONE"}).str, "OK");
  // The old link's sender must be gone, or it (online, acked) would be the replica counted below.
  ASSERT_TRUE(WaitFor([&] { return InfoField(*m, "replication", "connected_slaves") == 0; }, 10s));
  ASSERT_EQ(r->Command({"SET", "own", "1"}).str, "OK");
  ASSERT_EQ(r->Command({"CONFIG", "SET", "key-load-delay", "400"}).str, "OK");
  ASSERT_EQ(r->Command({"REPLICAOF", "127.0.0.1", std::to_string(master.port())}).str, "OK");
  ASSERT_TRUE(WaitFor([&] { return InfoField(*r, "persistence", "loading") == 1; }, 10s));
  const RespValue during = master.Connect()->Command({"WAIT", "1", "200"});
  ASSERT_EQ(during.type, RespValue::Type::kInteger) << during.str;
  EXPECT_EQ(during.integer, 0);  // woff 0, yet not online
  EXPECT_TRUE(WaitFor([&] { return master.Connect()->Command({"WAIT", "1", "100"}).integer == 1; }, 40s));
}

TEST_F(WaitIT, DemotionUnblocksAWaitWithUnblocked) {  // redis72-replication §4
  auto m = master.Connect();
  replica.Pause();
  m->Command({"INCR", "foo"});
  ASSERT_TRUE(m->Send({"WAIT", "1", "0"}));
  auto admin = master.Connect();
  ASSERT_TRUE(WaitFor([&] { return InfoField(*admin, "clients", "blocked_clients") == 1; }));
  ASSERT_EQ(admin->Command({"REPLICAOF", "127.0.0.1", "1"}).str, "OK");
  auto reply = m->Read();
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(reply->type, RespValue::Type::kError);
  EXPECT_EQ(reply->str,
            "UNBLOCKED force unblock from blocking operation, instance state changed (master -> replica?)");
  replica.Resume();
}

TEST_F(WaitIT, RefusedOnAReplica) {
  auto r = replica.Connect();
  const RespValue v = r->Command({"WAIT", "0", "0"});
  EXPECT_EQ(v.type, RespValue::Type::kError);
  EXPECT_TRUE(v.str.starts_with("ERR WAIT cannot be used with replica instances.")) << v.str;
}

}  // namespace
}  // namespace tideskv::test
