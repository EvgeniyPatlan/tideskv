#include "repl/replication_manager.h"

#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "encoding/replog_codec.h"
#include "exec/repl_log.h"
#include "repl/backlog_trimmer.h"
#include "repl/full_sync_frames.h"
#include "repl/socket_io.h"
#include "resp/request_parser.h"
#include "server/config.h"
#include "storage/memory_storage.h"
#include "support/command_harness.h"
#include "support/fake_clock.h"
#include "support/replica_stack.h"
#include "support/snapshot_frames.h"
#include "types/errors.h"

namespace tideskv::repl {
namespace {

using namespace std::chrono_literals;
using encoding::ReplOp;
using encoding::ReplOpType;

bool WaitFor(const std::function<bool()>& cond, std::chrono::milliseconds timeout = 2s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (cond()) return true;
    std::this_thread::sleep_for(5ms);
  }
  return cond();
}

// Reads until `needle` shows up, EOF, or the timeout. Returns everything read.
std::string ReadUntil(int fd, std::string_view needle, std::chrono::milliseconds timeout = 2s) {
  std::string out;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (out.find(needle) == std::string::npos && std::chrono::steady_clock::now() < deadline) {
    const Status st = ReadSome(fd, &out, 20ms);
    if (!st.ok() && st.code() != StatusCode::kTimedOut) break;
  }
  return out;
}

bool PeerClosed(int fd, std::chrono::milliseconds timeout = 2s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::string sink;
  while (std::chrono::steady_clock::now() < deadline) {
    const Status st = ReadSome(fd, &sink, 20ms);
    if (st.code() == StatusCode::kIOError) return true;
  }
  return false;
}

struct ManagerTest : ::testing::Test {
  ManagerTest() {
    EXPECT_TRUE(log.Recover().ok());
    for (int i = 0; i < 3; ++i) {
      EXPECT_TRUE(
          log.CommitStandalone({{ReplOpType::kPut, ColumnFamily::kMeta, "k" + std::to_string(i), "v"}}).ok());
    }
    id.replid = std::string(40, 'f');
  }
  std::unique_ptr<ReplicationManager> Make(std::chrono::milliseconds timeout = 60s) {
    ReplicationManager::Options o;
    o.repl_timeout = [timeout] { return timeout; };
    return Make(std::move(o));
  }
  std::unique_ptr<ReplicationManager> Make(ReplicationManager::Options o) {
    auto m = std::make_unique<ReplicationManager>(storage, log, id, std::move(o));
    m->Start();
    return m;
  }
  // Attaches a replica that asked for next_seq; returns the replica's end of the link.
  Fd Attach(ReplicationManager& m, uint64_t next_seq) {
    const PsyncResult r = m.HandlePsync({id.replid, next_seq}, {"10.0.0.9", 7009});
    EXPECT_EQ(r.decision, PsyncDecision::kContinue);
    int fds[2] = {-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    m.AttachReplica(r.pending_id, fds[0], "");
    return Fd(fds[1]);
  }

  InMemoryStorage storage;
  test::FakeClock clock;
  ReplLog log{storage, clock};
  ReplIdentity id;
};

TEST_F(ManagerTest, IsAMaster) {
  auto m = Make();
  EXPECT_EQ(m->role(), ReplRole::kMaster);
  EXPECT_FALSE(m->IsReplica());
  EXPECT_EQ(m->replid(), id.replid);
}

TEST_F(ManagerTest, HandlePsyncDecidesAndCounts) {
  auto m = Make();
  const PsyncResult cont = m->HandlePsync({id.replid, 2}, {});
  EXPECT_EQ(cont.decision, PsyncDecision::kContinue);
  EXPECT_EQ(cont.replid, id.replid);
  EXPECT_NE(cont.pending_id, 0u);
  const PsyncResult full = m->HandlePsync({std::string(40, 'a'), 2}, {});
  EXPECT_EQ(full.decision, PsyncDecision::kFullResync);
  EXPECT_EQ(full.replid, id.replid);
  EXPECT_EQ(full.seq, 3u);
  EXPECT_NE(full.pending_id, 0u);  // FULLRESYNC hands the socket over too (M6b)
  EXPECT_NE(full.pending_id, cont.pending_id);
  const PsyncResult unknown = m->HandlePsync({"?", 0}, {});
  EXPECT_EQ(unknown.decision, PsyncDecision::kFullResync);
  const ReplicationManager::Counters c = m->counters();
  EXPECT_EQ(c.sync_partial_ok, 1u);
  EXPECT_EQ(c.sync_full, 2u);
  EXPECT_EQ(c.sync_partial_err, 1u);  // only the request that named an id
}

TEST_F(ManagerTest, AttachStreamsFromTheRequestedSeq) {
  auto m = Make();
  Fd replica = Attach(*m, 2);
  const std::string got = ReadUntil(replica.get(), "\r\n$1\r\n3\r\n");
  EXPECT_EQ(got.find("$1\r\n1\r\n"), std::string::npos);
  EXPECT_NE(got.find("$1\r\n2\r\n"), std::string::npos);
  ASSERT_EQ(m->Replicas().size(), 1u);
  EXPECT_EQ(m->Replicas()[0].ip, "10.0.0.9");
  EXPECT_EQ(m->Replicas()[0].listening_port, 7009);
  EXPECT_EQ(m->counters().connected_replicas, 1u);
}

TEST_F(ManagerTest, AttachWithAnUnknownPendingIdClosesTheSocket) {
  auto m = Make();
  int fds[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  Fd replica(fds[1]);
  m->AttachReplica(12345, fds[0], "");
  EXPECT_TRUE(PeerClosed(replica.get()));
  EXPECT_TRUE(m->Replicas().empty());
}

TEST_F(ManagerTest, APendingIdIsUsedOnce) {
  auto m = Make();
  const PsyncResult r = m->HandlePsync({id.replid, 4}, {});
  int a[2] = {-1, -1};
  int b[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, a), 0);
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, b), 0);
  Fd ra(a[1]);
  Fd rb(b[1]);
  m->AttachReplica(r.pending_id, a[0], "");
  m->AttachReplica(r.pending_id, b[0], "");
  EXPECT_TRUE(PeerClosed(rb.get()));
  EXPECT_EQ(m->Replicas().size(), 1u);
}

TEST_F(ManagerTest, DeadSendersAreReaped) {
  auto m = Make();
  Fd replica = Attach(*m, 4);
  ASSERT_EQ(m->Replicas().size(), 1u);
  replica = Fd();
  EXPECT_TRUE(WaitFor([&] { return m->Replicas().empty(); }, 3s));
}

TEST_F(ManagerTest, StalePendingEntriesExpire) {
  auto m = Make(100ms);
  const PsyncResult r = m->HandlePsync({id.replid, 4}, {});
  EXPECT_EQ(m->pending_count(), 1u);
  EXPECT_TRUE(WaitFor([&] { return m->pending_count() == 0; }, 3s));
  int fds[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  Fd replica(fds[1]);
  m->AttachReplica(r.pending_id, fds[0], "");
  EXPECT_TRUE(PeerClosed(replica.get()));
}

TEST_F(ManagerTest, DropAllReplicasClosesEveryLink) {
  auto m = Make();
  Fd r1 = Attach(*m, 4);
  Fd r2 = Attach(*m, 4);
  ASSERT_EQ(m->Replicas().size(), 2u);
  m->DropAllReplicas();
  EXPECT_TRUE(m->Replicas().empty());
  EXPECT_TRUE(PeerClosed(r1.get()));
  EXPECT_TRUE(PeerClosed(r2.get()));
}

TEST_F(ManagerTest, ShutdownIsPromptAndIdempotent) {
  auto m = Make();
  Fd r1 = Attach(*m, 4);
  std::this_thread::sleep_for(50ms);
  const auto start = std::chrono::steady_clock::now();
  m->Shutdown();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 300ms);
  EXPECT_TRUE(PeerClosed(r1.get()));
  m->Shutdown();
  // A hand-off that completes after shutdown is refused.
  const PsyncResult r = m->HandlePsync({id.replid, 4}, {});
  int fds[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  Fd replica(fds[1]);
  m->AttachReplica(r.pending_id, fds[0], "");
  EXPECT_TRUE(PeerClosed(replica.get()));
  EXPECT_TRUE(m->Replicas().empty());
}

TEST_F(ManagerTest, DestructorShutsDown) {
  auto m = Make();
  Fd r1 = Attach(*m, 4);
  m.reset();
  EXPECT_TRUE(PeerClosed(r1.get()));
}

// F4: live replica links count against maxclients with the server's connections. A hand-off still in
// flight is one of those connections until its sender runs, so it is not counted twice; a PSYNC beyond
// the limit gets the server's maxclients error, with no decision taken and nothing registered.
TEST_F(ManagerTest, ReplicaLinksCountAgainstMaxclients) {
  uint64_t connected = 2;  // two PSYNC callers at once
  ReplicationManager::Options o;
  o.maxclients = [] { return uint64_t{2}; };
  o.connected_clients = [&connected] { return connected; };
  auto m = Make(std::move(o));
  std::vector<uint64_t> pending;
  for (int i = 0; i < 2; ++i) {  // 2 connections + 0 links: neither hand-off has happened yet
    const PsyncResult r = m->HandlePsync({id.replid, 4}, {});
    ASSERT_TRUE(r.error.empty()) << i << ": " << r.error;
    pending.push_back(r.pending_id);
  }
  std::vector<Fd> links;
  for (uint64_t pending_id : pending) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    links.emplace_back(fds[1]);
    m->AttachReplica(pending_id, fds[0], "");
  }
  connected = 1;                                                   // both handed off; a third caller
  const PsyncResult refused = m->HandlePsync({id.replid, 4}, {});  // 1 connection + 2 links
  EXPECT_EQ(refused.error, kErrMaxClients);
  EXPECT_EQ(refused.pending_id, 0u);
  EXPECT_EQ(m->pending_count(), 0u);
  EXPECT_EQ(m->counters().sync_partial_ok, 2u);
  EXPECT_EQ(m->counters().sync_full, 0u);
  links.pop_back();  // a dropped link frees its slot at once, before it is reaped
  EXPECT_TRUE(WaitFor([&] { return m->HandlePsync({id.replid, 4}, {}).error.empty(); }));
}

// F5: with an unknown baseline the master never continues a "?" request.
TEST_F(ManagerTest, AnUnknownBaselineAnswersQuestionMarkWithFullResync) {
  ReplicationManager::Options o;
  o.baseline_unknown = true;
  auto m = Make(std::move(o));
  EXPECT_EQ(m->HandlePsync({"?", 1}, {}).decision, PsyncDecision::kFullResync);
  EXPECT_EQ(m->HandlePsync({id.replid, 2}, {}).decision, PsyncDecision::kContinue);
  EXPECT_EQ(Make()->HandlePsync({"?", 1}, {}).decision, PsyncDecision::kContinue);
}

TEST_F(ManagerTest, FullResyncTakesTheSnapshotAndPinsTheBacklogAtIt) {
  auto m = Make();
  const PsyncResult full = m->HandlePsync({std::string(40, 'a'), 2}, {"10.0.0.8", 7008});
  ASSERT_EQ(full.decision, PsyncDecision::kFullResync);
  EXPECT_EQ(full.seq, 3u);
  EXPECT_NE(full.pending_id, 0u);
  EXPECT_EQ(m->OldestNeeded(), 4u);  // the hand-off needs S+1
  ASSERT_TRUE(log.CommitStandalone({{ReplOpType::kPut, ColumnFamily::kMeta, "late", "v"}}).ok());
  int fds[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  m->AttachReplica(full.pending_id, fds[0], "");
  Fd replica(fds[1]);
  const std::string got = ReadUntil(replica.get(), "$5\r\nBATCH\r\n$1\r\n4\r\n");
  EXPECT_NE(got.find("$7\r\nLOADEND\r\n$1\r\n3\r\n"), std::string::npos);
  ASSERT_EQ(m->Replicas().size(), 1u);
  EXPECT_FALSE(m->Replicas()[0].online);
  EXPECT_EQ(m->counters().sync_full, 1u);
  ASSERT_TRUE(WriteAll(replica.get(), RespCommand({"REPLCONF", "ACK", "3"}), 1s).ok());
  EXPECT_TRUE(WaitFor([&] { return m->Replicas()[0].online; }));
}

// A parked WAIT client for the manager's tests: its replies land in `link`.
std::shared_ptr<BlockedClient> WaitClient(BlockingRegistry& reg, const std::shared_ptr<test::FakeLink>& link,
                                          uint64_t id) {
  Session s;
  s.id = id;
  s.link = link;
  auto c =
      std::make_shared<BlockedClient>(reg, s, std::vector<std::string>{"WAIT"}, encoding::ValueType::kList,
                                      std::vector<std::string>{}, std::nullopt);
  reg.Add(c);
  return c;
}

// D9: an ACK answers the WAIT clients it satisfies, with the count; a waiter added after the ACK arrived
// is answered by AddWaiter itself.
TEST_F(ManagerTest, AnAckAnswersTheWaitClientsItSatisfies) {
  auto m = Make();
  Fd replica = Attach(*m, 4);
  EXPECT_EQ(m->CountAcked(0), 1);  // a partial-sync link is online at once
  EXPECT_EQ(m->CountAcked(3), 0);
  BlockingRegistry reg;
  auto link = std::make_shared<test::FakeLink>();
  auto two = WaitClient(reg, link, 1);
  auto one = WaitClient(reg, link, 2);
  m->AddWaiter(two, 3, 2);
  m->AddWaiter(one, 3, 1);
  EXPECT_FALSE(one->ended());
  ASSERT_TRUE(WriteAll(replica.get(), RespCommand({"REPLCONF", "ACK", "3"}), 1s).ok());
  ASSERT_TRUE(WaitFor([&] { return one->ended(); }));
  EXPECT_EQ(link->Take(), std::vector<std::string>{":1\r\n"});
  EXPECT_FALSE(two->ended());  // one replica of the two it asked for
  EXPECT_EQ(m->CountAcked(3), 1);
  ASSERT_TRUE(WriteAll(replica.get(), RespCommand({"REPLCONF", "ACK", "3"}), 1s).ok());  // CF-M6b-7
  auto late = WaitClient(reg, link, 3);
  m->AddWaiter(late, 3, 1);
  EXPECT_TRUE(late->ended());
  EXPECT_EQ(link->Take(), std::vector<std::string>{":1\r\n"});
  ASSERT_TRUE(two->Cancel());  // its timeout; the tick prunes it
  EXPECT_EQ(reg.blocked_clients(), 0u);
}

// D10: a full-sync link counts for WAIT only from its first ACK after LOADEND.
TEST_F(ManagerTest, AFullSyncLinkIsNotCountedUntilItAcks) {
  auto m = Make();
  const PsyncResult full = m->HandlePsync({"?", 0}, {});
  ASSERT_EQ(full.decision, PsyncDecision::kFullResync);
  int fds[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  m->AttachReplica(full.pending_id, fds[0], "");
  Fd replica(fds[1]);
  ASSERT_NE(ReadUntil(replica.get(), "$7\r\nLOADEND\r\n").find("LOADEND"), std::string::npos);
  EXPECT_EQ(m->CountAcked(0), 0);
  ASSERT_TRUE(WriteAll(replica.get(), RespCommand({"REPLCONF", "ACK", "3"}), 1s).ok());
  EXPECT_TRUE(WaitFor([&] { return m->CountAcked(3) == 1; }));
}

// D14: rdb-key-save-delay (OnConfig) slows every key a full-sync sender streams.
TEST_F(ManagerTest, KeySaveDelayThrottlesTheSnapshotStream) {
  auto m = Make();
  ConfigValues v;
  v.rdb_key_save_delay = 200000;  // 0.2 s per key, 3 keys
  m->OnConfig(v);
  const PsyncResult full = m->HandlePsync({"?", 0}, {});
  ASSERT_EQ(full.decision, PsyncDecision::kFullResync);
  int fds[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  m->AttachReplica(full.pending_id, fds[0], "");
  Fd replica(fds[1]);
  EXPECT_EQ(ReadUntil(replica.get(), "LOADEND", 300ms).find("LOADEND"), std::string::npos);
  EXPECT_NE(ReadUntil(replica.get(), "LOADEND", 3s).find("LOADEND"), std::string::npos);
}

// Task 14: what the backlog trimmer may not delete, and the drop of replicas beyond the limits.
TEST_F(ManagerTest, OldestNeededIsTheMinOverReplicasAndHandOffs) {
  auto m = Make();
  EXPECT_EQ(m->OldestNeeded(), kNoReplicaNeeds);
  Fd r1 = Attach(*m, 4);
  EXPECT_EQ(m->OldestNeeded(), 4u);  // holds 3, has not acknowledged yet
  Fd r2 = Attach(*m, 2);
  EXPECT_EQ(m->OldestNeeded(), 2u);
  ASSERT_NE(ReadUntil(r2.get(), "\r\n$1\r\n3\r\n").find("$1\r\n3\r\n"), std::string::npos);
  ASSERT_TRUE(WriteAll(r2.get(), RespCommand({"REPLCONF", "ACK", "3"}), 1s).ok());
  EXPECT_TRUE(WaitFor([&] { return m->OldestNeeded() == 4u; }));  // the ACK raised it
  m->HandlePsync({id.replid, 3}, {});                             // a hand-off that has not attached yet
  EXPECT_EQ(m->OldestNeeded(), 3u);
}

TEST_F(ManagerTest, DropReplicasNeedingClosesOnlyTheLaggingLinks) {
  auto m = Make();
  Fd ahead = Attach(*m, 4);
  Fd behind = Attach(*m, 2);
  m->HandlePsync({id.replid, 3}, {});
  m->DropReplicasNeeding(2);
  EXPECT_TRUE(PeerClosed(behind.get()));
  ASSERT_EQ(m->Replicas().size(), 1u);
  EXPECT_EQ(m->pending_count(), 1u);
  m->DropReplicasNeeding(3);  // the hand-off for 3 is forgotten; the replica needing 4 stays
  EXPECT_EQ(m->pending_count(), 0u);
  EXPECT_EQ(m->Replicas().size(), 1u);
  EXPECT_EQ(m->OldestNeeded(), 4u);
}

// The hooks a role change runs, recorded in order with the fence state each one saw.
struct RoleTest : ManagerTest {
  std::unique_ptr<ReplicationManager> MakeWithHooks() {
    ReplicationManager::Options o;
    o.role.stop_reaper = [this] { Record("stop_reaper"); };
    o.role.start_reaper = [this] { Record("start_reaper"); };
    o.role.disconnect_blocked = [this](std::string_view e) {
      Record("disconnect");
      error = std::string(e);
    };
    o.role.reinit_oids = [this] {
      Record("reinit_oids");
      return Status::Ok();
    };
    o.role.set_replicaof = [this](const std::string& v) { Record("replicaof=" + v); };
    auto m = std::make_unique<ReplicationManager>(storage, log, id, std::move(o));
    m->Start();
    return m;
  }
  void Record(const std::string& what) {
    std::lock_guard lock(mu);
    events.push_back(what + (log.fenced() ? " fenced" : ""));
  }
  std::vector<std::string> Take() {
    std::lock_guard lock(mu);
    return std::exchange(events, {});
  }

  std::mutex mu;
  std::vector<std::string> events;
  std::string error;
};

TEST_F(RoleTest, BecomeReplicaFencesThenStopsTheReaperUnblocksAndDropsReplicas) {
  auto m = MakeWithHooks();
  Fd sub = Attach(*m, 4);
  EXPECT_TRUE(m->BecomeReplica("Master.Example", 6379));
  EXPECT_TRUE(log.fenced());
  EXPECT_EQ(m->role(), ReplRole::kReplica);
  EXPECT_EQ(Take(), (std::vector<std::string>{"stop_reaper fenced", "disconnect fenced",
                                              "replicaof=Master.Example 6379 fenced"}));
  EXPECT_EQ(error,
            "UNBLOCKED force unblock from blocking operation, instance state changed (master -> replica?)");
  EXPECT_TRUE(m->Replicas().empty());
  EXPECT_TRUE(PeerClosed(sub.get()));
  EXPECT_FALSE(m->BecomeReplica("master.example", 6379));  // the same master, case-insensitively
  EXPECT_TRUE(Take().empty());
  EXPECT_TRUE(m->BecomeReplica("master.example", 6380));  // another master: the steps run again
  EXPECT_EQ(Take().size(), 3u);
}

// F8: M6a has no chained replication. A replica refuses PSYNC with an error (no decision, nothing
// registered), and becoming a replica forgets the hand-offs a master had registered.
TEST_F(RoleTest, AReplicaRefusesPsyncAndForgetsPendingHandOffs) {
  auto m = MakeWithHooks();
  const PsyncResult pending = m->HandlePsync({id.replid, 4}, {});
  ASSERT_EQ(pending.decision, PsyncDecision::kContinue);
  ASSERT_EQ(m->pending_count(), 1u);
  m->BecomeReplica("h", 1);
  EXPECT_EQ(m->pending_count(), 0u);
  int fds[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  Fd late(fds[1]);
  m->AttachReplica(pending.pending_id, fds[0], "");  // the hand-off completes too late: closed
  EXPECT_TRUE(PeerClosed(late.get()));
  EXPECT_TRUE(m->Replicas().empty());
  const PsyncResult refused = m->HandlePsync({id.replid, 4}, {});
  EXPECT_EQ(refused.error, kErrPsyncOnReplica);
  EXPECT_EQ(refused.pending_id, 0u);
  EXPECT_EQ(m->pending_count(), 0u);
  EXPECT_EQ(m->counters().sync_partial_ok, 1u);  // the one before the demotion
  ASSERT_TRUE(m->BecomeMaster().ok());
  EXPECT_TRUE(m->HandlePsync({m->replid(), 4}, {}).error.empty());
}

TEST_F(RoleTest, RoleReportsTheMasterAndALinkThatIsDown) {
  auto m = MakeWithHooks();
  RoleInfo r = m->Role();
  EXPECT_EQ(r.role, ReplRole::kMaster);
  EXPECT_EQ(r.offset, 3u);
  m->BecomeReplica("10.1.1.1", 7000);
  r = m->Role();
  EXPECT_EQ(r.role, ReplRole::kReplica);
  EXPECT_EQ(r.host, "10.1.1.1");
  EXPECT_EQ(r.port, 7000);
  EXPECT_EQ(r.state, ReplicaLinkState::kConnect);  // no engine here: no client runs
  EXPECT_EQ(r.offset_or_minus1, -1);
}

TEST_F(RoleTest, BecomeMasterPromotesTheIdentityThenUnfencesAndStartsTheReaper) {
  auto m = MakeWithHooks();
  EXPECT_TRUE(m->BecomeMaster().ok());  // a no-op on a master
  EXPECT_TRUE(Take().empty());
  EXPECT_EQ(m->replid(), id.replid);
  m->BecomeReplica("h", 1);
  Take();
  EXPECT_TRUE(m->BecomeMaster().ok());
  EXPECT_EQ(Take(), (std::vector<std::string>{"reinit_oids fenced", "start_reaper", "replicaof="}));
  EXPECT_FALSE(log.fenced());
  EXPECT_EQ(m->role(), ReplRole::kMaster);
  const ReplIdentity now = m->identity();
  EXPECT_NE(now.replid, id.replid);
  EXPECT_TRUE(IsValidReplId(now.replid));
  ASSERT_EQ(now.history.size(), 1u);
  EXPECT_EQ(now.history[0], (HistoryEntry{id.replid, 3}));
  auto persisted = LoadOrCreateIdentity(storage);
  ASSERT_TRUE(persisted.ok());
  EXPECT_EQ(persisted->replid, now.replid);
  EXPECT_EQ(persisted->history, now.history);
  EXPECT_EQ(m->Role().role, ReplRole::kMaster);
}

TEST_F(RoleTest, RoleChangesAfterShutdownStartNothing) {
  auto m = MakeWithHooks();
  m->Shutdown();
  m->BecomeReplica("h", 1);
  EXPECT_TRUE(m->BecomeMaster().ok());
  EXPECT_TRUE(Take().empty());
  EXPECT_FALSE(log.fenced());
}

TEST(ReplicationManagerStale, RefusesOnlyAsADisconnectedReplicaWithServeStaleNo) {
  InMemoryStorage s;
  test::FakeClock clock;
  ReplLog log(s, clock);
  ASSERT_TRUE(log.Recover().ok());
  ReplicationManager m(s, log, ReplIdentity{std::string(kReplIdLength, 'a'), {}});
  ConfigValues v;
  v.replica_serve_stale_data = false;
  m.OnConfig(v);
  EXPECT_FALSE(m.StaleRefusing());               // a master is never stale
  ASSERT_TRUE(m.BecomeReplica("127.0.0.1", 1));  // no engine hook: no client starts, the link stays down
  EXPECT_TRUE(m.StaleRefusing());
  v.replica_serve_stale_data = true;
  m.OnConfig(v);
  EXPECT_FALSE(m.StaleRefusing());
}

TEST(ReplicationManagerStale, ConcurrentConfigSetsLeaveTheFinalValue) {
  InMemoryStorage s;
  test::FakeClock clock;
  ReplLog log(s, clock);
  ASSERT_TRUE(log.Recover().ok());
  ReplicationManager m(s, log, ReplIdentity{std::string(kReplIdLength, 'a'), {}});
  ASSERT_TRUE(m.BecomeReplica("127.0.0.1", 1));
  Config c;
  c.SetOnChange([&](const ConfigValues&) { m.RefreshConfig([&] { return c.Snapshot(); }); });
  for (int round = 0; round < 200; ++round) {
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
      threads.emplace_back([&, t] {
        for (int i = 0; i < 20; ++i) {
          ASSERT_TRUE(c.Set("replica-serve-stale-data", ((t + i) % 2 == 0) ? "no" : "yes", true).ok());
        }
      });
    }
    for (auto& th : threads) th.join();
    ASSERT_EQ(m.StaleRefusing(), !c.Snapshot().replica_serve_stale_data) << "round " << round;
  }
}

// A one-shot master on 127.0.0.1: accepts the replica, answers its handshake up to PSYNC.
class OneLinkMaster {
 public:
  OneLinkMaster() {
    listen_ = Fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    EXPECT_EQ(::bind(listen_.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    EXPECT_EQ(::listen(listen_.get(), 4), 0);
    EXPECT_EQ(::getsockname(listen_.get(), reinterpret_cast<sockaddr*>(&addr), &len), 0);
    port_ = ntohs(addr.sin_port);
  }
  uint16_t port() const { return port_; }
  // Accepts, then replies +PONG / +OK until PSYNC, which gets `psync_reply`. False on a timeout.
  bool Serve(const std::string& psync_reply) {
    pollfd p{listen_.get(), POLLIN, 0};
    if (::poll(&p, 1, 3000) != 1) return false;
    link_ = Fd(::accept4(listen_.get(), nullptr, nullptr, SOCK_CLOEXEC));
    std::string in;
    resp::RequestParser parser;
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
      size_t consumed = 0;
      const auto ps = parser.Parse(in, &consumed);
      in.erase(0, consumed);
      if (ps == resp::ParseStatus::kCommand) {
        const std::string name = parser.args()[0];
        if (name == "PSYNC") return Send("+" + psync_reply + "\r\n");
        Send(name == "PING" ? "+PONG\r\n" : "+OK\r\n");
        continue;
      }
      const Status st = ReadSome(link_.get(), &in, 20ms);
      if (!st.ok() && st.code() != StatusCode::kTimedOut) return false;
    }
    return false;
  }
  bool Send(std::string_view bytes) { return WriteAll(link_.get(), bytes, 2s).ok(); }

 private:
  Fd listen_;
  Fd link_;
  uint16_t port_ = 0;
};

// M6b D5, D11: the manager's client loads a full sync: Sync() and ROLE report it while it runs; the load
// publishes the master's replid and clears an unknown baseline.
TEST(ReplicationManagerFullSync, ReportsTheLoadAndAdoptsItsResult) {
  test::CommandHarness master;
  for (int i = 0; i < 20; ++i) ASSERT_EQ(master.Run({"SET", "k" + std::to_string(i), "v"}), "+OK\r\n");
  const std::string master_id = master.services.replication_.identity.replid;
  InMemoryStorage storage;
  test::ReplicaStack replica(storage);
  ReplicationManager::Options o;
  o.baseline_unknown = true;
  o.role.engine = &replica.engine;
  ReplicationManager m(storage, replica.log, ReplIdentity{std::string(kReplIdLength, 'a'), {}}, std::move(o));
  EXPECT_EQ(m.Sync().loading_start_ms, 0);  // no client yet
  OneLinkMaster fake;
  ASSERT_TRUE(m.BecomeReplica("127.0.0.1", fake.port()));
  ASSERT_TRUE(fake.Serve("FULLRESYNC " + master_id + " " + std::to_string(master.services.repl_log_.last())));
  const test::SnapshotStream snap = test::SnapshotFrames(master.services.repl_log_);
  std::string chunks;
  for (size_t i = 0; i + 1 < snap.frames.size(); ++i) chunks += LoadFrame(snap.frames[i][1]);
  ASSERT_TRUE(fake.Send(chunks));
  ASSERT_TRUE(WaitFor([&] { return m.Sync().loaded_bytes > 0; }));
  const ReplicationManager::SyncProgress p = m.Sync();
  EXPECT_TRUE(p.in_progress);
  EXPECT_GT(p.read_bytes, 0u);
  EXPECT_GT(p.last_io_ms, 0);
  EXPECT_GT(p.loading_start_ms, 0);
  EXPECT_EQ(m.Role().state, ReplicaLinkState::kSync);
  EXPECT_EQ(m.Role().offset_or_minus1, -1);
  EXPECT_FALSE(m.baseline_unknown());  // the start txn deleted the flag
  EXPECT_TRUE(m.identity().history.empty());
  ASSERT_TRUE(fake.Send(LoadEndFrame(snap.seq, snap.frames.back()[2], std::stoull(snap.frames.back()[3]))));
  ASSERT_TRUE(WaitFor([&] { return m.Role().state == ReplicaLinkState::kConnected; }));
  EXPECT_FALSE(m.Sync().in_progress);
  EXPECT_EQ(m.Sync().loaded_bytes, 0u);
  EXPECT_EQ(m.replid(), master_id);
  EXPECT_EQ(test::DigestOf(replica.engine), test::DigestOf(master.services.engine_));
  m.Shutdown();
}

}  // namespace
}  // namespace tideskv::repl
