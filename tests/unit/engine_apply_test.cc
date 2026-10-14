#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "encoding/key_codec.h"
#include "exec/engine.h"
#include "expire/reaper.h"
#include "storage/memory_storage.h"
#include "support/command_harness.h"
#include "support/replica_stack.h"
#include "types/errors.h"
#include "types/oid_allocator.h"
#include "types/string_type.h"

namespace tideskv {
namespace {

using namespace std::chrono_literals;
using test::CommandHarness;
using test::DigestOf;
using test::ReplicaStack;
using test::ReplogEntry;
using test::ShipAll;

const std::string kEmptyDigest(40, '0');

// M6a D11: a replica applies the master's replog batches; master and replica are two stacks.
struct EngineApplyTest : ::testing::Test {
  CommandHarness master;
  InMemoryStorage replica_storage;
  ReplicaStack replica{replica_storage};

  Storage& master_storage() { return master.services.storage_; }
  uint64_t master_last() { return master.services.repl_log_.last(); }
  void Run(std::initializer_list<std::string_view> argv) {
    const std::string reply = master.Run(argv);
    ASSERT_FALSE(reply.starts_with("-")) << *argv.begin() << ": " << reply;
  }
  Status Ship() { return ShipAll(master_storage(), master_last(), replica.engine); }
  std::string Payload(uint64_t seq) {
    auto p = ReplogEntry(master_storage(), seq);
    EXPECT_TRUE(p.ok()) << p.status().ToString();
    return p.ok() ? *p : std::string();
  }
  bool ReplicaHas(uint8_t db, const std::string& key) {
    auto view = replica_storage.BeginRead();
    return (*view)->Get(ColumnFamily::kMeta, encoding::MetaKeyFor(db, key)).ok();
  }
};

TEST_F(EngineApplyTest, ReplicaConvergesToMasterDigest) {
  Run({"SET", "s", "v", "EX", "100"});
  Run({"HSET", "h", "f1", "v1", "f2", "v2"});
  Run({"LPUSH", "l", "a", "b", "c"});
  Run({"ZADD", "z", "1", "a", "2.5", "b"});
  Run({"SADD", "set", "x", "y"});
  Run({"HSET", "gone", "f", "v"});
  Run({"DEL", "gone"});
  Run({"EXPIRE", "l", "500"});
  Run({"SELECT", "1"});
  Run({"SET", "d1", "v"});
  Run({"HSET", "d1h", "f", "v"});
  Run({"FLUSHDB"});
  Run({"SELECT", "2"});
  Run({"SADD", "d2", "m"});
  Run({"SELECT", "0"});
  Run({"MULTI"});
  Run({"SET", "m1", "v"});
  Run({"INCR", "counter"});
  Run({"HSET", "h", "f3", "v3"});
  Run({"EXEC"});
  Run({"EVAL", "redis.call('set', KEYS[1], ARGV[1]) redis.call('rpush', KEYS[2], 'x') return 1", "2", "e",
       "l2", "v"});
  const uint64_t n = master_last();
  ASSERT_GE(n, 10u);
  ASSERT_TRUE(Ship().ok());
  EXPECT_EQ(replica.log.last(), n);
  const std::string digest = DigestOf(master.services.engine_);
  EXPECT_NE(digest, kEmptyDigest);
  EXPECT_EQ(DigestOf(replica.engine), digest);
  for (uint64_t seq = 1; seq <= n; ++seq) {
    auto mine = ReplogEntry(replica_storage, seq);
    ASSERT_TRUE(mine.ok()) << seq;
    EXPECT_EQ(*mine, Payload(seq)) << "replog[" << seq << "]";
  }
  EXPECT_EQ(replica.log.cum_bytes(), master.services.repl_log_.cum_bytes());
}

TEST_F(EngineApplyTest, GapIsRefusedAndNothingApplied) {
  Run({"SET", "a", "1"});
  Run({"SET", "b", "2"});
  EXPECT_EQ(replica.engine.ApplyReplicated(2, Payload(2)).code(), StatusCode::kInternal);
  EXPECT_EQ(replica.log.last(), 0u);
  EXPECT_EQ(DigestOf(replica.engine), kEmptyDigest);
  EXPECT_FALSE(ReplicaHas(0, "b"));
}

TEST_F(EngineApplyTest, CorruptPayloadIsRefusedAndNothingApplied) {
  Run({"SET", "a", "1"});
  std::string payload = Payload(1);
  payload[payload.size() / 2] ^= 0x01;
  EXPECT_EQ(replica.engine.ApplyReplicated(1, payload).code(), StatusCode::kCorruption);
  EXPECT_EQ(replica.log.last(), 0u);
  EXPECT_EQ(DigestOf(replica.engine), kEmptyDigest);
  EXPECT_TRUE(replica.engine.ApplyReplicated(1, Payload(1)).ok());  // the intact batch still applies
}

TEST_F(EngineApplyTest, ApplyWithoutAReplLogIsAnInternalError) {
  Run({"SET", "a", "1"});
  LockManager locks;
  test::FakeClock clock;
  Engine plain(replica_storage, locks, clock);
  EXPECT_EQ(plain.ApplyReplicated(1, Payload(1)).code(), StatusCode::kInternal);
  EXPECT_FALSE(ReplicaHas(0, "a"));
}

// CF9: the apply's shared gate with no key stripes is safe only while the fence keeps every local writer
// out, so an apply on an unfenced ReplLog is refused and applies nothing.
TEST_F(EngineApplyTest, ApplyRequiresTheFenceRaised) {
  Run({"SET", "a", "1"});
  replica.log.FenceLocalWrites(false);
  EXPECT_EQ(replica.engine.ApplyReplicated(1, Payload(1)).code(), StatusCode::kInternal);
  EXPECT_EQ(replica.log.last(), 0u);
  EXPECT_FALSE(ReplicaHas(0, "a"));
  replica.log.FenceLocalWrites(true);
  EXPECT_TRUE(replica.engine.ApplyReplicated(1, Payload(1)).ok());
  EXPECT_TRUE(ReplicaHas(0, "a"));
}

TEST_F(EngineApplyTest, ApplyInvalidatesWatchesOnTheReplica) {
  auto client = std::make_shared<WatchClient>();
  replica.watches.Watch(client, 0, "k", false);
  Run({"SET", "other", "v"});
  ASSERT_TRUE(Ship().ok());
  EXPECT_FALSE(client->dirty());
  Run({"SET", "k", "v"});
  ASSERT_TRUE(Ship().ok());
  EXPECT_TRUE(client->dirty());
}

TEST_F(EngineApplyTest, ApplyOfAFlushInvalidatesWatchesOfTheFlushedDb) {
  Run({"SELECT", "1"});
  Run({"SET", "k", "v"});
  ASSERT_TRUE(Ship().ok());
  auto in_db1 = std::make_shared<WatchClient>();
  auto in_db0 = std::make_shared<WatchClient>();
  replica.watches.Watch(in_db1, 1, "k", false);
  replica.watches.Watch(in_db0, 0, "k", false);
  Run({"FLUSHDB"});
  ASSERT_TRUE(Ship().ok());
  EXPECT_TRUE(in_db1->dirty());
  EXPECT_FALSE(in_db0->dirty());  // db 0 was not flushed and has no "k"
  Run({"SET", "k", "v"});
  Run({"SELECT", "0"});
  Run({"SET", "k", "v"});
  ASSERT_TRUE(Ship().ok());
  auto watcher = std::make_shared<WatchClient>();
  replica.watches.Watch(watcher, 0, "k", false);
  Run({"FLUSHALL"});
  ASSERT_TRUE(Ship().ok());
  EXPECT_TRUE(watcher->dirty());
}

TEST_F(EngineApplyTest, ApplyFeedsTheCommitListener) {
  Run({"HSET", "h", "f", "v"});
  ASSERT_TRUE(Ship().ok());
  EXPECT_TRUE(replica.listener.seen.empty());
  Run({"DEL", "h"});
  ASSERT_TRUE(Ship().ok());
  ASSERT_FALSE(replica.listener.seen.empty());
  bool subkey_prefix = false;
  for (const RangeDelete& d : replica.listener.seen) {
    subkey_prefix |= d.cf == ColumnFamily::kSubkey && d.is_prefix;
  }
  EXPECT_TRUE(subkey_prefix);
}

TEST_F(EngineApplyTest, LocalWritesAreFencedButApplyIsNot) {
  ASSERT_TRUE(replica.log.fenced());
  Run({"SET", "due", "v", "PX", "10"});
  Run({"HSET", "h", "f", "v"});  // the master leases an oid block first
  ASSERT_TRUE(Ship().ok());
  const Status local = replica.engine.Write(0, {"x"}, [](WriteTxn& t, int64_t now) {
    SetResult out;
    return StringSet(t, 0, "x", "v", SetOptions{}, now, &out);
  });
  EXPECT_EQ(local.code(), StatusCode::kUserError);
  EXPECT_EQ(local.message(), kErrReadOnlyReplica);
  EXPECT_FALSE(ReplicaHas(0, "x"));
  replica.clock.Advance(1000);  // "due" is past its deadline on the replica too
  ExpireReaper reaper(replica.engine, ExpireReaper::Options{});
  auto reaped = reaper.RunOnce();
  ASSERT_TRUE(reaped.ok()) << reaped.status().ToString();  // a replica waits for the master's DEL
  EXPECT_EQ(*reaped, 0u);
  EXPECT_TRUE(ReplicaHas(0, "due"));
  OidAllocator oids(replica_storage, {}, [&](const std::vector<encoding::ReplOp>& ops) {
    return replica.log.CommitStandalone(ops);
  });
  ASSERT_TRUE(oids.Init().ok());
  const auto lease = oids.Next();  // nothing leased yet: a lease boundary
  EXPECT_EQ(lease.status().code(), StatusCode::kUserError);
  EXPECT_EQ(lease.status().message(), kErrReadOnlyReplica);
  const uint64_t before = replica.log.last();
  Run({"SET", "after", "v"});
  ASSERT_TRUE(Ship().ok());
  EXPECT_EQ(replica.log.last(), before + 1);
  EXPECT_TRUE(ReplicaHas(0, "after"));
}

TEST_F(EngineApplyTest, ApplyWaitsThroughABusyScript) {
  Run({"SET", "k", "v"});
  const std::string payload = Payload(1);
  replica.engine.SetBusyGuard([] { return true; }, "BUSY test");
  auto batch = replica.engine.BeginBatch();  // a timed-out script holds the gate
  ASSERT_TRUE(batch.ok());
  std::atomic<bool> done{false};
  Status applied;
  std::thread applier([&] {
    applied = replica.engine.ApplyReplicated(1, payload);
    done = true;
  });
  std::this_thread::sleep_for(30ms);
  replica.engine.WakeWaiters();  // the script timeout's wake-up must not make it give up
  std::this_thread::sleep_for(30ms);
  EXPECT_FALSE(done.load());
  batch->reset();  // the script ends
  applier.join();
  EXPECT_TRUE(applied.ok()) << applied.ToString();
  EXPECT_EQ(replica.log.last(), 1u);
  EXPECT_TRUE(ReplicaHas(0, "k"));
}

}  // namespace
}  // namespace tideskv
