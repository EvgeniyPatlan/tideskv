#include <gtest/gtest.h>

#include <string>

#include "cluster/cluster_errors.h"
#include "cluster/slot_states.h"
#include "support/cluster_harness.h"
#include "types/errors.h"

namespace tideskv::test {
namespace {

const std::string kMovedFooToB = "MOVED 12182 127.0.0.1:7001";
const std::string kCrossSlot = "CROSSSLOT Keys in request don't hash to the same slot";
const std::string kNotServed = "CLUSTERDOWN Hash slot not served";
const std::string kExecAbort = "EXECABORT Transaction discarded because of previous errors.";
const std::string kAllOnB =
    kNodeA + " 127.0.0.1 7000 master -\n" + kNodeB + " 127.0.0.1 7001 master - 0-16383\n";

// This harness is node A of kThreeNodes, topology version 1.
struct RoutedA : CommandHarness {
  RoutedA() : cluster(*this) { cluster.Push(kThreeNodes, 1); }
  ClusterHarness cluster;
};

TEST(ClusterRouting, KeylessCommandsAreNeverRouted) {
  CommandHarness h;
  ClusterHarness c(h);  // uninitialised: every keyed command is CLUSTERDOWN
  EXPECT_EQ(h.Run({"PING"}), "+PONG\r\n");
  EXPECT_EQ(h.Run({"DBSIZE"}), Int(0));
  EXPECT_EQ(h.Run({"KEYS", "*"}), "*0\r\n");
  EXPECT_EQ(h.Run({"GET", "b"}), Err(kNotServed));  // gocase "requests on non-init-cluster"
}

TEST(ClusterRouting, OwnedSlotsRunAndForeignSlotsAreMoved) {
  RoutedA h;
  EXPECT_EQ(h.Run({"SET", "b", "1"}), kOk);
  EXPECT_EQ(h.Run({"SET", "foo", "1"}), Err(kMovedFooToB));
  EXPECT_EQ(h.Run({"GET", "foo"}), Err(kMovedFooToB));
}

TEST(ClusterRouting, UnservedSlotsAreClusterdown) {
  CommandHarness h;
  ClusterHarness c(h);
  c.Push(kNodeA + " 127.0.0.1 7000 master - 0-8191\n", 1);
  EXPECT_EQ(h.Run({"GET", "foo"}), Err(kNotServed));
}

TEST(ClusterRouting, CrossSlotIsRefusedEvenWhenEverySlotIsMine) {
  RoutedA h;
  EXPECT_EQ(h.Run({"MSET", "b", "1", "bar", "2"}), Err(kCrossSlot));  // both on A, different slots
  EXPECT_EQ(h.Run({"MSET", "{b}1", "1", "{b}2", "2"}), kOk);
  EXPECT_EQ(h.Run({"ZUNIONSTORE", "b", "2", "{b}1", "foo"}), Err(kCrossSlot));  // D17: the sources count
  EXPECT_EQ(h.Run({"EVAL", "return 1", "2", "b", "foo"}), Err(kCrossSlot));
  EXPECT_EQ(h.Run({"WATCH", "foo"}), Err(kMovedFooToB));
}

TEST(ClusterRouting,
     AQueueTimeRefusalDoomsTheTransaction) {  // gocase "MULTI-exec cross slots but not in one node"
  RoutedA h;
  ASSERT_EQ(h.Run({"MULTI"}), kOk);
  EXPECT_EQ(h.Run({"SET", "b", "multi"}), "+QUEUED\r\n");
  EXPECT_EQ(h.Run({"SET", "foo", "x"}), Err(kMovedFooToB));
  EXPECT_EQ(h.Run({"EXEC"}), Err(kExecAbort));
  EXPECT_EQ(h.Run({"GET", "b"}), kNil);
}

TEST(ClusterRouting, ExecRoutesTheWholeQueueAndRepliesABareError) {  // D14, server.c:3959-3966
  RoutedA h;
  ASSERT_EQ(h.Run({"MULTI"}), kOk);
  ASSERT_EQ(h.Run({"SET", "b", "1"}), "+QUEUED\r\n");
  ASSERT_EQ(h.Run({"SET", "bar", "2"}), "+QUEUED\r\n");
  EXPECT_EQ(h.Run({"EXEC"}), Err(kCrossSlot));
  EXPECT_FALSE(h.session.multi.active);  // discarded
  EXPECT_EQ(h.Run({"GET", "b"}), kNil);
}

TEST(ClusterRouting, ExecAfterATopologyChangeIsMoved) {
  RoutedA h;
  ASSERT_EQ(h.Run({"MULTI"}), kOk);
  ASSERT_EQ(h.Run({"SET", "b", "1"}), "+QUEUED\r\n");
  h.cluster.Push(kAllOnB, 2);
  EXPECT_EQ(h.Run({"EXEC"}), Err("MOVED 3300 127.0.0.1:7001"));
}

TEST(ClusterRouting, AReplicaMovesUntilReadonlyAndNeverServesWrites) {  // gocase "requests on cluster are ok"
  CommandHarness h;
  ClusterHarness c(h, 7002);  // node C, A's replica
  c.Push(kThreeNodes, 1);
  const std::string moved_to_a = "MOVED 3300 127.0.0.1:7000";
  EXPECT_EQ(h.Run({"GET", "b"}), Err(moved_to_a));
  EXPECT_EQ(h.Run({"READONLY"}), kOk);
  EXPECT_EQ(h.Run({"GET", "b"}), kNil);
  EXPECT_EQ(h.Run({"SET", "b", "1"}), Err(moved_to_a));      // MOVED comes before READONLY (D13)
  EXPECT_EQ(h.Run({"GET", "foo"}), Err(kMovedFooToB));       // not my master's slot
  EXPECT_EQ(h.Run({"EVAL", "return 1", "1", "b"}), Int(1));  // a compat EVAL is not a write (Redis)
  EXPECT_EQ(h.Run({"EVAL", "#!lua\nreturn 1", "1", "b"}), Err(moved_to_a));  // shebang without no-writes
  EXPECT_EQ(h.Run({"READWRITE"}), kOk);
  EXPECT_EQ(h.Run({"GET", "b"}), Err(moved_to_a));
}

TEST(ClusterRouting,
     ReadonlyTransactionsOnAReplicaRunAndQueuedWritesMove) {  // 16-transactions-on-replica.tcl
  CommandHarness h;
  ClusterHarness c(h, 7002);
  c.Push(kThreeNodes, 1);
  ASSERT_EQ(h.Run({"READONLY"}), kOk);
  ASSERT_EQ(h.Run({"MULTI"}), kOk);
  ASSERT_EQ(h.Run({"GET", "b"}), "+QUEUED\r\n");
  EXPECT_EQ(h.Run({"EXEC"}), Arr({kNil}));
  ASSERT_EQ(h.Run({"MULTI"}), kOk);
  EXPECT_EQ(h.Run({"SET", "b", "1"}), Err("MOVED 3300 127.0.0.1:7000"));
  EXPECT_EQ(h.Run({"EXEC"}), Err(kExecAbort));
}

TEST(ClusterRouting, ARouterNodeMovesEverything) {  // gocase "requests non-member of cluster"
  CommandHarness h;
  ClusterHarness c(h, 7999);  // in no line of the topology
  c.Push(kThreeNodes, 1);
  EXPECT_EQ(h.Run({"SET", "b", "1"}), Err("MOVED 3300 127.0.0.1:7000"));
  EXPECT_EQ(h.Run({"GET", "foo"}), Err(kMovedFooToB));
}

TEST(ClusterRouting, AskingLastsForTheNextCommandOrTheWholeTransaction) {  // networking.c:2100-2103
  RoutedA h;
  EXPECT_EQ(h.Run({"ASKING"}), kOk);
  EXPECT_TRUE(h.session.asking);
  EXPECT_EQ(h.Run({"PING"}), "+PONG\r\n");
  EXPECT_FALSE(h.session.asking);
  ASSERT_EQ(h.Run({"ASKING"}), kOk);
  EXPECT_EQ(h.Run({"GET", "foo"}), Err(kMovedFooToB));  // cleared after a refused command too
  EXPECT_FALSE(h.session.asking);
  ASSERT_EQ(h.Run({"ASKING"}), kOk);
  ASSERT_EQ(h.Run({"MULTI"}), kOk);
  ASSERT_EQ(h.Run({"GET", "b"}), "+QUEUED\r\n");
  EXPECT_TRUE(h.session.asking);
  ASSERT_EQ(h.Run({"EXEC"}), Arr({kNil}));
  EXPECT_FALSE(h.session.asking);
}

TEST(ClusterRouting, ResetClearsReadonlyAndAsking) {
  RoutedA h;
  ASSERT_EQ(h.Run({"READONLY"}), kOk);
  ASSERT_EQ(h.Run({"ASKING"}), kOk);
  ASSERT_EQ(h.Run({"RESET"}), "+RESET\r\n");
  EXPECT_FALSE(h.session.readonly_reads);
  EXPECT_FALSE(h.session.asking);
}

TEST(ClusterRouting, ClusterOnlyCommandsAreRefusedOutsideClusterMode) {  // cluster.c:7284-7306
  CommandHarness h;
  for (const char* cmd : {"READONLY", "READWRITE", "ASKING"}) {
    EXPECT_EQ(h.Run({cmd}), Err("ERR This instance has cluster support disabled")) << cmd;
  }
}

TEST(ClusterRouting, SelectIsDbZeroOnly) {  // db.c:762-765, D18
  RoutedA h;
  EXPECT_EQ(h.Run({"SELECT", "0"}), kOk);
  EXPECT_EQ(h.Run({"SELECT", "1"}), Err("ERR SELECT is not allowed in cluster mode"));
  EXPECT_EQ(h.Run({"SELECT", "x"}), Err("ERR value is not an integer or out of range"));
  EXPECT_NE(
      h.Run({"EVAL", "return redis.call('select','2')", "0"}).find("SELECT is not allowed in cluster mode"),
      std::string::npos);
}

// Review Focus #1.
TEST(ClusterRouting, TheNextCommandSeesANewTopology) {
  RoutedA h;
  ASSERT_EQ(h.Run({"SET", "b", "1"}), kOk);
  h.cluster.Push(kAllOnB, 2);
  EXPECT_EQ(h.Run({"GET", "b"}), Err("MOVED 3300 127.0.0.1:7001"));
  h.cluster.Push(kThreeNodes, 3);
  EXPECT_EQ(h.Run({"GET", "b"}), Bulk("1"));
}

// Review Focus #2: CLIENT PAUSE re-dispatches from the start, so a write paused on a master that SETNODES
// demoted meanwhile (the controller main's manual failover) is MOVED and never applied here.
TEST(ClusterRouting, APausedWriteIsReroutedAfterADemotion) {
  RoutedA h;
  SecondClient writer(h);
  ASSERT_EQ(h.Run({"CLIENT", "PAUSE", "60000", "WRITE"}), kOk);
  EXPECT_EQ(writer.Run({"SET", "b", "paused"}), "");
  h.cluster.Push(kNodeA + " 127.0.0.1 7000 slave " + kNodeC + "\n" + kNodeB +
                     " 127.0.0.1 7001 master - 8192-16383\n" + kNodeC + " 127.0.0.1 7002 master - 0-8191\n",
                 2);
  EXPECT_EQ(h.services.replication_.role(), ReplRole::kReplica);
  ASSERT_EQ(h.Run({"CLIENT", "UNPAUSE"}), kOk);
  EXPECT_EQ(writer.RunPostponed(), Err("MOVED 3300 127.0.0.1:7002"));
  EXPECT_TRUE(h.Cf(ColumnFamily::kMeta).empty());
}

// Review Focus #4: Redis keyHashSlot through the whole dispatch path (A owns every slot here, so only a
// CROSSSLOT can refuse).
TEST(ClusterRouting, HashTagEdgeCasesRouteLikeRedis) {
  CommandHarness h;
  ClusterHarness c(h);
  c.Push(kNodeA + " 127.0.0.1 7000 master - 0-16383\n", 1);
  EXPECT_EQ(h.Run({"MSET", "{a}x", "1", "{a}y", "2", "a", "3"}), kOk);  // tag "a" is key "a"
  EXPECT_EQ(h.Run({"MSET", "{{a}", "1", "{a", "2"}), kOk);              // tag "{a" is key "{a" (10276)
  EXPECT_EQ(h.Run({"MSET", "x{a}{b}", "1", "a", "2"}), kOk);            // only the first tag counts
  EXPECT_EQ(h.Run({"MSET", "{}", "1", "a{}b", "2"}),
            Err(kCrossSlot));  // empty tags: whole keys (15257, 13694)
  const std::string nul("k\0x", 3);
  EXPECT_EQ(h.Run({"MSET", "{" + nul + "}1", "1", nul, "2"}), kOk);  // binary tags (5125)
  EXPECT_EQ(h.Run({"CLUSTER", "KEYSLOT", "{{}}"}), Int(4092));       // the tag is "{"
}

// Review Focus #4: the fence lands between queueing and EXEC. Route still passes (the states are not
// published yet, as in the instant between the barrier and the publish), so EXEC itself must refuse, as a
// whole.
TEST(ClusterRouting, AFencedSlotFailsAWholeExec) {
  RoutedA h;
  ASSERT_EQ(h.Run({"MULTI"}), kOk);
  ASSERT_EQ(h.Run({"SET", "b", "1"}), "+QUEUED\r\n");
  ASSERT_EQ(h.Run({"INCR", "{b}n"}), "+QUEUED\r\n");
  h.services.engine_.FenceSlots(3300, 3300);
  EXPECT_EQ(h.Run({"EXEC"}), Err(kErrTryAgain));  // one error reply, never an EXEC array
  EXPECT_FALSE(h.session.multi.active);
  h.services.engine_.UnfenceSlots(3300, 3300);
  EXPECT_EQ(h.Run({"GET", "b"}), kNil);
  EXPECT_EQ(h.Run({"GET", "{b}n"}), kNil);
  ASSERT_EQ(h.Run({"MULTI"}), kOk);  // a transaction that only reads a fenced slot runs
  ASSERT_EQ(h.Run({"GET", "b"}), "+QUEUED\r\n");
  h.services.engine_.FenceSlots(3300, 3300);
  EXPECT_EQ(h.Run({"EXEC"}), Arr({kNil}));
}

TEST(ClusterRouting, AFencedSlotFailsAWholeEval) {  // D4
  RoutedA h;
  h.services.engine_.FenceSlots(3300, 3300);
  EXPECT_EQ(
      h.Run({"EVAL", "redis.call('set', KEYS[1], 'x') return redis.call('incr', KEYS[2])", "2", "b", "{b}n"}),
      Err(kErrTryAgain));
  EXPECT_EQ(h.Run({"EVAL_RO", "return redis.call('get', KEYS[1])", "1", "b"}), kNil);
  EXPECT_EQ(h.Run({"EVAL", "#!lua flags=no-writes\nreturn redis.call('get', KEYS[1])", "1", "b"}), kNil);
  h.services.engine_.UnfenceSlots(3300, 3300);
  EXPECT_EQ(h.Run({"GET", "b"}), kNil);
  EXPECT_EQ(h.Run({"GET", "{b}n"}), kNil);
}

TEST(ClusterRouting, ScriptCallsToAFencedSlotAnswerTryagain) {  // a fenced slot is local, not non-local
  RoutedA h;
  auto s = std::make_shared<cluster::SlotStates>();
  s->fenced.set(3300);
  h.cluster.manager->PublishStates(s);
  EXPECT_EQ(h.Run({"SET", "b", "1"}), Err(cluster::kErrTryAgainFenced));  // Route's fast path
  EXPECT_NE(h.Run({"EVAL", "return redis.call('set', 'b', 'x')", "0"}).find("TRYAGAIN"), std::string::npos);
  EXPECT_EQ(h.Run({"GET", "b"}), kNil);
}

}  // namespace
}  // namespace tideskv::test
