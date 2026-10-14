#include "repl/replication_manager.h"

#include <gtest/gtest.h>
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
#include "repl/socket_io.h"
#include "storage/memory_storage.h"
#include "support/fake_clock.h"

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
  m->BecomeMaster();  // a no-op on a master
  EXPECT_TRUE(Take().empty());
  EXPECT_EQ(m->replid(), id.replid);
  m->BecomeReplica("h", 1);
  Take();
  m->BecomeMaster();
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
  m->BecomeMaster();
  EXPECT_TRUE(Take().empty());
  EXPECT_FALSE(log.fenced());
}

}  // namespace
}  // namespace tideskv::repl
