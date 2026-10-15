// A port of the Redis replication-2.tcl min-slaves cases (M6b D12).
#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "support/it_helpers.h"
#include "support/repl_pair.h"
#include "support/test_server.h"

namespace tideskv::test {
namespace {

using namespace std::chrono_literals;

struct MinReplicasIT : ReplPairIT {};

TEST_F(MinReplicasIT, HonouredWhenEnoughAndRefusedWhenNot) {
  auto m = master.Connect();
  ASSERT_EQ(m->Command({"CONFIG", "SET", "min-slaves-to-write", "1"}).str, "OK");
  ASSERT_EQ(m->Command({"CONFIG", "SET", "min-slaves-max-lag", "10"}).str, "OK");
  ASSERT_TRUE(WaitFor([&] { return m->Command({"SET", "foo", "bar"}).str == "OK"; }));
  EXPECT_EQ(InfoField(*m, "replication", "min_slaves_good_slaves"), 1);
  ASSERT_EQ(m->Command({"CONFIG", "SET", "min-slaves-to-write", "2"}).str, "OK");
  EXPECT_EQ(m->Command({"SET", "foo", "bar"}).str, "NOREPLICAS Not enough good replicas to write.");
  EXPECT_EQ(m->Command({"EVAL", "return redis.call('get','foo')", "0"}).str, "bar");  // reads in Lua work
}

TEST_F(MinReplicasIT, ALaggingReplicaIsNotGood) {  // replication-2.tcl: pause the replica process
  auto m = master.Connect();
  ASSERT_EQ(m->Command({"CONFIG", "SET", "min-slaves-to-write", "1"}).str, "OK");
  ASSERT_EQ(m->Command({"CONFIG", "SET", "min-slaves-max-lag", "2"}).str, "OK");
  ASSERT_TRUE(WaitFor([&] { return m->Command({"SET", "foo", "bar"}).str == "OK"; }));
  replica.Pause();
  EXPECT_TRUE(
      WaitFor([&] { return m->Command({"SET", "foo", "bar"}).type == RespValue::Type::kError; }, 10s));
  replica.Resume();
  EXPECT_TRUE(WaitFor([&] { return m->Command({"SET", "foo", "bar"}).str == "OK"; }, 10s));
}

TEST_F(MinReplicasIT, IgnoredByReplicas) {  // replication-2.tcl "min-slaves-to-write is ignored by slaves"
  auto r = replica.Connect();
  ASSERT_EQ(r->Command({"CONFIG", "SET", "min-slaves-to-write", "1"}).str, "OK");
  ASSERT_EQ(r->Command({"CONFIG", "SET", "min-slaves-max-lag", "10"}).str, "OK");
  auto m = master.Connect();
  ASSERT_EQ(m->Command({"SET", "foo", "12345"}).str, "OK");
  EXPECT_TRUE(WaitFor([&] { return r->Command({"GET", "foo"}).str == "12345"; }));
}

// CF-M6b-10: after REPLICAOF NO ONE the old link must drop before replicas are counted.
TEST_F(MinReplicasIT, AReplicaThatLeftIsNoLongerCounted) {
  auto m = master.Connect();
  ASSERT_EQ(m->Command({"CONFIG", "SET", "min-slaves-to-write", "1"}).str, "OK");
  ASSERT_TRUE(WaitFor([&] { return m->Command({"SET", "foo", "bar"}).str == "OK"; }));
  ASSERT_EQ(replica.Connect()->Command({"REPLICAOF", "NO", "ONE"}).str, "OK");
  ASSERT_TRUE(WaitFor([&] { return InfoField(*m, "replication", "connected_slaves") == 0; }));
  EXPECT_EQ(m->Command({"SET", "foo", "bar"}).str, "NOREPLICAS Not enough good replicas to write.");
  EXPECT_EQ(InfoField(*m, "replication", "min_slaves_good_slaves"), 0);
}

}  // namespace
}  // namespace tideskv::test
