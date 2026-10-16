#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "repl/replication_manager.h"
#include "script/sha1hex.h"
#include "support/command_harness.h"
#include "support/replica_stack.h"
#include "types/errors.h"

namespace tideskv::test {
namespace {

std::string ScriptSuffix(const std::string& code) {
  return " script: " + Sha1Hex(code) + ", on @user_script:1.";
}

struct ReplHarness : CommandHarness {
  ReplHarness() {
    for (int i = 0; i < 3; ++i) Run({"SET", "k" + std::to_string(i), "v"});
  }
  const std::string& myid() const { return services.replication_.identity.replid; }
  uint64_t last() const { return services.repl_log_.last(); }
};

TEST(PsyncCmd, NotAllowedInsideMulti) {
  ReplHarness h;
  EXPECT_EQ(h.Run({"MULTI"}), kOk);
  EXPECT_EQ(h.Run({"PSYNC", h.myid(), "1"}), Err("ERR Command not allowed inside a transaction"));
  EXPECT_EQ(h.Run({"SYNC"}), Err("ERR Command not allowed inside a transaction"));
  EXPECT_EQ(h.Run({"EXEC"}), Err("EXECABORT Transaction discarded because of previous errors."));
  EXPECT_FALSE(h.session.takeover);
}

TEST(PsyncCmd, NotAllowedFromScripts) {
  ReplHarness h;
  for (const std::string c : {"return redis.call('psync', '?', '-1')", "return redis.call('sync')",
                              "return redis.call('replconf')"}) {
    EXPECT_EQ(h.Run({"EVAL", c, "0"}),
              Err("ERR This Redis command is not allowed from script" + ScriptSuffix(c)));
  }
}

TEST(PsyncCmd, WrongArity) {
  ReplHarness h;
  EXPECT_EQ(h.Run({"PSYNC", "?"}), Err("ERR wrong number of arguments for 'psync' command"));
  EXPECT_EQ(h.Run({"SYNC", "x"}), Err("ERR wrong number of arguments for 'sync' command"));
}

TEST(PsyncCmd, NonIntegerOffsetKeepsAnOrdinaryClient) {
  ReplHarness h;
  EXPECT_EQ(h.Run({"PSYNC", h.myid(), "abc"}), Err("ERR value is not an integer or out of range"));
  EXPECT_FALSE(h.session.takeover);
  EXPECT_FALSE(h.session.close_after_reply);
  EXPECT_EQ(h.Run({"PING"}), "+PONG\r\n");
}

TEST(PsyncCmd, ContinueSetsTheTakeover) {
  ReplHarness h;
  ASSERT_EQ(h.last(), 3u);
  EXPECT_EQ(h.Run({"PSYNC", h.myid(), "1"}), "+CONTINUE " + h.myid() + "\r\n");
  ASSERT_TRUE(h.session.takeover);
  EXPECT_TRUE(h.session.is_replica_link);
  EXPECT_FALSE(h.session.close_after_reply);
  auto& fake = h.services.replication_;
  ASSERT_EQ(fake.psyncs.size(), 1u);
  EXPECT_EQ(fake.psyncs[0].next_seq, 1u);
  int fds[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  h.session.takeover(fds[0], "unread");
  ::close(fds[1]);
  ASSERT_EQ(fake.attached.size(), 1u);
  EXPECT_EQ(fake.attached[0].pending_id, fake.last_pending_id);
  EXPECT_EQ(fake.attached[0].unread, "unread");
}

TEST(PsyncCmd, ContinueAtTheEndOfTheLogAndFromAnEmptyReplica) {
  ReplHarness h;
  EXPECT_EQ(h.Run({"PSYNC", h.myid(), "4"}), "+CONTINUE " + h.myid() + "\r\n");
  ReplHarness h2;
  EXPECT_EQ(h2.Run({"psync", "?", "1"}), "+CONTINUE " + h2.myid() + "\r\n");
}

TEST(PsyncCmd, FullResyncRepliesTheIdAndSeqThenHandsTheSocketOver) {
  ReplHarness h;
  EXPECT_EQ(h.Run({"PSYNC", std::string(40, 'a'), "2"}),
            "+FULLRESYNC " + h.myid() + " " + std::to_string(h.last()) + "\r\n");
  EXPECT_EQ(h.last(), 3u);
  EXPECT_FALSE(h.session.close_after_reply);
  EXPECT_TRUE(h.session.takeover);
  EXPECT_TRUE(h.session.is_replica_link);
  ReplHarness h2;
  EXPECT_EQ(h2.Run({"PSYNC", "?", "-1"}), "+FULLRESYNC " + h2.myid() + " 3\r\n");  // redis-cli style
  ReplHarness h3;
  EXPECT_EQ(h3.Run({"PSYNC", h3.myid(), "5"}), "+FULLRESYNC " + h3.myid() + " 3\r\n");  // beyond last+1
}

TEST(PsyncCmd, FailoverIsRefusedOnAMaster) {
  ReplHarness h;
  EXPECT_EQ(h.Run({"PSYNC", h.myid(), "1", "FAILOVER"}),
            Err("ERR PSYNC FAILOVER can't be sent to a master."));
  EXPECT_FALSE(h.session.takeover);
}

TEST(PsyncCmd, IgnoredOnAReplicaLink) {
  ReplHarness h;
  h.session.is_replica_link = true;
  EXPECT_EQ(h.Run({"PSYNC", h.myid(), "1"}), "");
  EXPECT_EQ(h.Run({"SYNC"}), "");
  EXPECT_TRUE(h.services.replication_.psyncs.empty());
}

TEST(PsyncCmd, PassesTheReplconfStateAsThePeer) {
  ReplHarness h;
  h.session.peer = "10.1.2.3:5555";
  EXPECT_EQ(h.Run({"REPLCONF", "listening-port", "7000"}), kOk);
  h.Run({"PSYNC", h.myid(), "1"});
  auto& fake = h.services.replication_;
  EXPECT_EQ(fake.last_peer.ip, "10.1.2.3");
  EXPECT_EQ(fake.last_peer.listening_port, 7000);
  ReplHarness h2;
  h2.session.peer = "10.1.2.3:5555";
  EXPECT_EQ(h2.Run({"REPLCONF", "ip-address", "replica.example", "listening-port", "7001"}), kOk);
  h2.Run({"PSYNC", h2.myid(), "1"});
  EXPECT_EQ(h2.services.replication_.last_peer.ip, "replica.example");
  EXPECT_EQ(h2.services.replication_.last_peer.listening_port, 7001);
}

// F4 / F8: a refused PSYNC (maxclients, a replica) answers the error and keeps an ordinary client.
TEST(PsyncCmd, ARefusalRepliesTheErrorWithoutAHandOff) {
  ReplHarness h;
  h.services.replication_.psync_error = "ERR max number of clients reached";
  EXPECT_EQ(h.Run({"PSYNC", h.myid(), "1"}), Err("ERR max number of clients reached"));
  EXPECT_FALSE(h.session.takeover);
  EXPECT_FALSE(h.session.is_replica_link);
  EXPECT_FALSE(h.session.close_after_reply);
  EXPECT_EQ(h.Run({"PING"}), "+PONG\r\n");
}

TEST(SyncCmd, IsNotSupported) {
  ReplHarness h;
  EXPECT_EQ(h.Run({"SYNC"}), Err("ERR SYNC is not supported, use PSYNC"));
  EXPECT_FALSE(h.session.close_after_reply);
}

TEST(ReplconfCmd, ListeningPortAndCapabilities) {
  ReplHarness h;
  EXPECT_EQ(h.Run({"REPLCONF", "listening-port", "7000", "capa", "psync2", "capa", "eof"}), kOk);
  EXPECT_EQ(h.session.replica_handshake.listening_port, 7000);
  EXPECT_EQ(h.Run({"REPLCONF", "capa", "unknown-capa"}), kOk);  // unknown capabilities are ignored
  EXPECT_EQ(h.Run({"REPLCONF"}), kOk);
  EXPECT_EQ(h.Run({"replconf", "LISTENING-PORT", "99999"}), kOk);  // no range check (Redis)
  auto& fake = h.services.replication_;
  ASSERT_GE(fake.replconfs.size(), 1u);
  EXPECT_EQ(fake.replconfs[0].listening_port, 7000);
  EXPECT_EQ(fake.replconfs[0].capa, (std::vector<std::string>{"psync2", "eof"}));
}

TEST(ReplconfCmd, Errors) {
  ReplHarness h;
  EXPECT_EQ(h.Run({"REPLCONF", "listening-port", "abc"}), Err("ERR value is not an integer or out of range"));
  EXPECT_EQ(h.Run({"REPLCONF", "foo", "bar"}), Err("ERR Unrecognized REPLCONF option: foo"));
  EXPECT_EQ(h.Run({"REPLCONF", "listening-port"}), Err("ERR syntax error"));
  EXPECT_EQ(h.Run({"REPLCONF", "a", "b", "c"}), Err("ERR syntax error"));
  // An error after a valid pair keeps what the valid pair set (options apply one by one, as in Redis).
  EXPECT_EQ(h.Run({"REPLCONF", "listening-port", "7002", "foo", "x"}),
            Err("ERR Unrecognized REPLCONF option: foo"));
  EXPECT_EQ(h.session.replica_handshake.listening_port, 7002);
}

TEST(ReplconfCmd, AckAndGetackNeverReply) {
  ReplHarness h;
  EXPECT_EQ(h.Run({"REPLCONF", "ACK", "5"}), "");
  EXPECT_EQ(h.Run({"REPLCONF", "GETACK", "*"}), "");
  EXPECT_EQ(h.Run({"REPLCONF", "listening-port", "1", "ack", "5"}), "");  // ack ends the command silently
  EXPECT_EQ(h.Run({"PING"}), "+PONG\r\n");
}

TEST(ReplconfCmd, IpAddressLengthLimit) {
  ReplHarness h;
  EXPECT_EQ(h.Run({"REPLCONF", "ip-address", std::string(256, 'a')}),
            Err("ERR REPLCONF ip-address provided by replica instance is too long: 256 bytes"));
  EXPECT_EQ(h.Run({"REPLCONF", "ip-address", std::string(255, 'a')}), kOk);
  EXPECT_EQ(h.session.replica_handshake.ip_address, std::string(255, 'a'));
}

TEST(ReplconfCmd, AllowedWhileAScriptIsBusyAndQueuedInMulti) {
  ReplHarness h;
  const CommandSpec* spec = h.services.registry().Find("replconf");
  ASSERT_NE(spec, nullptr);
  EXPECT_NE(spec->flags & kCmdAllowBusy, 0u);
  EXPECT_EQ(h.Run({"MULTI"}), kOk);
  EXPECT_EQ(h.Run({"REPLCONF", "capa", "eof"}), "+QUEUED\r\n");
  EXPECT_EQ(h.Run({"EXEC"}), Arr({kOk}));
}

}  // namespace
}  // namespace tideskv::test

namespace tideskv::test {
namespace {

const std::string kAlready = "+OK Already connected to specified master\r\n";
const std::string kUnblocked =
    "-UNBLOCKED force unblock from blocking operation, instance state changed (master -> replica?)\r\n";

TEST(ReplicaofCmd, ArityAndPortTexts) {
  CommandHarness h;
  EXPECT_EQ(h.Run({"REPLICAOF"}), Err("ERR wrong number of arguments for 'replicaof' command"));
  EXPECT_EQ(h.Run({"REPLICAOF", "NO", "ONE", "x"}),
            Err("ERR wrong number of arguments for 'replicaof' command"));
  EXPECT_EQ(h.Run({"SLAVEOF", "x"}), Err("ERR wrong number of arguments for 'slaveof' command"));
  for (const char* port : {"70000", "65536", "-1", "abc", ""}) {
    EXPECT_EQ(h.Run({"REPLICAOF", "h", port}), Err("ERR Invalid master port")) << port;
  }
  EXPECT_TRUE(h.services.replication_.become_replica.empty());
}

TEST(ReplicaofCmd, BecomesAReplicaAndReportsAlreadyConnected) {
  CommandHarness h;
  auto& fake = h.services.replication_;
  EXPECT_EQ(h.Run({"REPLICAOF", "Master.Example", "0"}), kOk);  // port 0 is accepted, the host never checked
  EXPECT_EQ(h.Run({"replicaof", "master.example", "0"}), kAlready);  // case-insensitive host
  EXPECT_EQ(h.Run({"SLAVEOF", "master.example", "65535"}), kOk);
  EXPECT_EQ(fake.become_replica, (std::vector<std::pair<std::string, uint16_t>>{{"Master.Example", 0},
                                                                                {"master.example", 65535}}));
  EXPECT_TRUE(fake.IsReplica());
}

TEST(ReplicaofCmd, NoOneIsCaseInsensitiveAndAnswersOkOnAMaster) {
  CommandHarness h;
  auto& fake = h.services.replication_;
  EXPECT_EQ(h.Run({"REPLICAOF", "no", "one"}), kOk);
  EXPECT_EQ(h.Run({"REPLICAOF", "h", "1"}), kOk);
  EXPECT_EQ(h.Run({"SLAVEOF", "No", "One"}), kOk);
  EXPECT_EQ(fake.become_master, 2);
  EXPECT_FALSE(fake.IsReplica());
}

TEST(ReplicaofCmd, RefusedOnAReplicaLink) {
  CommandHarness h;
  h.session.is_replica_link = true;
  EXPECT_EQ(h.Run({"REPLICAOF", "h", "1"}), Err("ERR Command is not valid when client is a replica."));
  EXPECT_TRUE(h.services.replication_.become_replica.empty());
}

TEST(ReplicaofCmd, QueuedInMultiAndAppliedAfterExec) {
  CommandHarness h;
  auto& fake = h.services.replication_;
  EXPECT_EQ(h.Run({"MULTI"}), kOk);
  EXPECT_EQ(h.Run({"REPLICAOF", "h", "1"}), "+QUEUED\r\n");
  EXPECT_FALSE(fake.IsReplica());
  EXPECT_EQ(h.Run({"EXEC"}), Arr({kOk}));
  EXPECT_TRUE(fake.IsReplica());
  EXPECT_EQ(h.Run({"MULTI"}), kOk);
  EXPECT_EQ(h.Run({"REPLICAOF", "H", "1"}), "+QUEUED\r\n");
  EXPECT_EQ(h.Run({"REPLICAOF", "no", "one"}), "+QUEUED\r\n");
  EXPECT_EQ(h.Run({"EXEC"}), Arr({kAlready, kOk}));
  EXPECT_FALSE(fake.IsReplica());
}

TEST(ReplicaofCmd, NotAllowedFromScripts) {
  CommandHarness h;
  for (const std::string c : {"return redis.call('replicaof', 'no', 'one')",
                              "return redis.call('slaveof', 'h', '1')", "return redis.call('role')"}) {
    EXPECT_EQ(h.Run({"EVAL", c, "0"}), Err("ERR This Redis command is not allowed from script script: " +
                                           Sha1Hex(c) + ", on @user_script:1."));
  }
}

TEST(RoleCmd, MasterShapes) {
  CommandHarness h;
  EXPECT_EQ(h.Run({"ROLE"}), "*3\r\n$6\r\nmaster\r\n:0\r\n*0\r\n");
  EXPECT_EQ(h.Run({"ROLE", "x"}), Err("ERR wrong number of arguments for 'role' command"));
  h.Run({"SET", "a", "1"});
  h.Run({"SET", "b", "2"});
  h.services.replication_.role_info.replicas = {{"10.0.0.1", 7000, 2, 0, true, true},
                                                {"10.0.0.2", 7001, 1, 0, false, true},
                                                {"10.0.0.3", 7002, 0, 0, true, false}};
  // Only live replicas that finished their full sync (online), with port and offset as bulk strings
  // (addReplyBulkLongLong).
  EXPECT_EQ(h.Run({"ROLE"}),
            "*3\r\n$6\r\nmaster\r\n:2\r\n*1\r\n*3\r\n$8\r\n10.0.0.1\r\n$4\r\n7000\r\n$1\r\n2\r\n");
  const CommandSpec* spec = h.services.registry().Find("role");
  ASSERT_NE(spec, nullptr);
  EXPECT_NE(spec->flags & kCmdFast, 0u);
}

TEST(RoleCmd, ReplicaShapes) {
  CommandHarness h;
  EXPECT_EQ(h.Run({"REPLICAOF", "nonexistent-host", "6379"}), kOk);
  EXPECT_EQ(h.Run({"ROLE"}),
            "*5\r\n$5\r\nslave\r\n$16\r\nnonexistent-host\r\n:6379\r\n$7\r\nconnect\r\n:-1\r\n");
  auto& info = h.services.replication_.role_info;
  info.state = ReplicaLinkState::kConnected;
  info.offset_or_minus1 = 43;
  EXPECT_EQ(h.Run({"ROLE"}),
            "*5\r\n$5\r\nslave\r\n$16\r\nnonexistent-host\r\n:6379\r\n$9\r\nconnected\r\n:43\r\n");
}

// The real ReplicationManager over the harness's in-memory stack, with the App's hooks wired to the
// harness. Its replica client points at a port where nothing listens, so it never applies anything.
struct RoleHarness : CommandHarness {
  RoleHarness() {
    auto id = repl::LoadOrCreateIdentity(services.storage_);
    EXPECT_TRUE(id.ok());
    repl::ReplicationManager::Options o;
    o.role.engine = &services.engine_;
    o.role.config = [this] { return services.config_.Snapshot(); };
    o.role.stop_reaper = [this] { reaper_running = false; };
    o.role.start_reaper = [this] { reaper_running = true; };
    o.role.disconnect_blocked = [this](std::string_view e) { services.blocking_.DisconnectAll(e); };
    o.role.reinit_oids = [this] {
      return fail_reinit ? Status::IOError("injected oid failure") : services.oids_.Init();
    };
    o.role.set_replicaof = [this](const std::string& v) {
      EXPECT_TRUE(services.config_.Set("replicaof", v.empty() ? "no one" : v, false).ok());
    };
    manager =
        std::make_unique<repl::ReplicationManager>(services.storage_, services.repl_log_, *id, std::move(o));
    services.real_replication = manager.get();
  }
  ~RoleHarness() { manager->Shutdown(); }

  bool reaper_running = true;
  std::atomic<bool> fail_reinit{false};
  std::unique_ptr<repl::ReplicationManager> manager;
};

// Fix round 1: a promotion that cannot persist its identity or reload the oid hwm leaves the node a
// fenced replica of the same master, and REPLICAOF NO ONE answers the storage error.
void ExpectStillAReplica(RoleHarness& h) {
  EXPECT_EQ(h.Run({"SET", "a", "2"}), Err("READONLY You can't write against a read only replica."));
  EXPECT_EQ(h.Run({"ROLE"}).substr(0, 14), "*5\r\n$5\r\nslave\r");
  EXPECT_TRUE(h.services.repl_log_.fenced());
  EXPECT_FALSE(h.reaper_running);
  EXPECT_EQ(h.services.config_.Snapshot().replicaof, "127.0.0.1 1");
  const RoleInfo r = h.manager->Role();
  EXPECT_EQ(r.host, "127.0.0.1");
  EXPECT_EQ(r.port, 1);
}

TEST(RoleChange, APromotionWhoseIdentityCannotBePersistedStaysAReplica) {
  RoleHarness h;
  const std::string old_id = h.manager->replid();
  EXPECT_EQ(h.Run({"REPLICAOF", "127.0.0.1", "1"}), kOk);
  h.services.storage_.fail_commit_in = 1;  // PersistIdentity's commit
  const std::string reply = h.Run({"REPLICAOF", "NO", "ONE"});
  EXPECT_TRUE(reply.starts_with("-ERR REPLICAOF NO ONE failed, still a replica: ")) << reply;
  EXPECT_NE(reply.find("injected commit failure"), std::string::npos) << reply;
  ExpectStillAReplica(h);
  EXPECT_EQ(h.manager->replid(), old_id);
  EXPECT_EQ(h.Run({"REPLICAOF", "NO", "ONE"}), kOk);  // the storage recovered: promotion now succeeds
  EXPECT_EQ(h.Run({"SET", "a", "3"}), kOk);
}

TEST(RoleChange, APromotionThatCannotReloadTheOidHwmStaysAReplica) {
  RoleHarness h;
  EXPECT_EQ(h.Run({"REPLICAOF", "127.0.0.1", "1"}), kOk);
  h.fail_reinit = true;
  const std::string reply = h.Run({"REPLICAOF", "NO", "ONE"});
  EXPECT_TRUE(reply.starts_with("-ERR REPLICAOF NO ONE failed, still a replica: ")) << reply;
  EXPECT_NE(reply.find("injected oid failure"), std::string::npos) << reply;
  ExpectStillAReplica(h);
  h.fail_reinit = false;
  EXPECT_EQ(h.Run({"REPLICAOF", "NO", "ONE"}), kOk);
}

TEST(RoleChange, DemotionUnblocksBlockedClientsAndClosesThem) {
  RoleHarness h;
  SecondClient b(h);
  EXPECT_EQ(b.Run({"BLPOP", "list", "0"}), "");
  ASSERT_TRUE(b.session.blocked);
  EXPECT_EQ(h.Run({"REPLICAOF", "127.0.0.1", "1"}), kOk);
  EXPECT_EQ(b.TakeUnblocked(), (std::vector<std::string>{kUnblocked}));
  EXPECT_EQ(b.link->closes(), 1);
  EXPECT_EQ(h.services.blocking_.blocked_clients(), 0u);
  EXPECT_FALSE(h.reaper_running);
}

TEST(RoleChange, AReplicaIsReadOnlyUntilPromoted) {
  RoleHarness h;
  EXPECT_EQ(h.Run({"SET", "a", "1"}), kOk);
  const std::string old_id = h.manager->replid();
  const uint64_t last = h.services.repl_log_.last();
  EXPECT_EQ(h.Run({"REPLICAOF", "127.0.0.1", "1"}), kOk);
  EXPECT_EQ(h.services.config_.Snapshot().replicaof, "127.0.0.1 1");
  EXPECT_EQ(h.Run({"SET", "a", "2"}), Err("READONLY You can't write against a read only replica."));
  EXPECT_EQ(h.Run({"REPLICAOF", "127.0.0.1", "1"}), kAlready);
  EXPECT_EQ(h.Run({"ROLE"}).substr(0, 14), "*5\r\n$5\r\nslave\r");
  EXPECT_EQ(h.Run({"REPLICAOF", "NO", "ONE"}), kOk);
  EXPECT_EQ(h.services.config_.Snapshot().replicaof, "");
  EXPECT_TRUE(h.reaper_running);
  EXPECT_EQ(h.Run({"SET", "a", "3"}), kOk);
  EXPECT_NE(h.manager->replid(), old_id);  // INFO's master_replid; replid2 is checked in Task 13
  const repl::ReplIdentity now = h.manager->identity();
  ASSERT_FALSE(now.history.empty());
  EXPECT_EQ(now.history[0], (repl::HistoryEntry{old_id, last}));
  auto persisted = repl::LoadOrCreateIdentity(h.services.storage_);
  ASSERT_TRUE(persisted.ok());
  EXPECT_EQ(persisted->replid, now.replid);
  EXPECT_EQ(h.Run({"ROLE"}), "*3\r\n$6\r\nmaster\r\n:" + std::to_string(last + 1) + "\r\n*0\r\n");
}

// CF7: a promoted replica allocates object ids above the high-water mark it applied from its master.
TEST(RoleChange, APromotedReplicaLeasesOidsAboveTheMastersHighWaterMark) {
  CommandHarness master;
  EXPECT_EQ(master.Run({"HSET", "h", "f", "v"}), Int(1));  // leases the master's first oid block
  const uint64_t master_hwm = master.services.oids_.hwm();
  ASSERT_GT(master_hwm, 1u);
  RoleHarness replica;
  EXPECT_EQ(replica.Run({"REPLICAOF", "127.0.0.1", "1"}), kOk);
  ASSERT_TRUE(
      ShipAll(master.services.storage_, master.services.repl_log_.last(), replica.services.engine_).ok());
  EXPECT_EQ(replica.Run({"REPLICAOF", "NO", "ONE"}), kOk);
  auto oid = replica.services.oids_.Next();
  ASSERT_TRUE(oid.ok()) << oid.status().ToString();
  EXPECT_GE(*oid, master_hwm);
}

TEST(ReplicaofCmd, APromotionRefusedWithLoadingRepliesLoadingVerbatim) {  // M6b D7, M7a D22
  CommandHarness h;
  h.services.replication_.set_role(ReplRole::kReplica);
  h.services.replication_.become_master_status = Status::UserError(std::string(kErrLoading));
  EXPECT_EQ(h.Run({"REPLICAOF", "NO", "ONE"}), Err(kErrLoading));
  EXPECT_EQ(h.services.replication_.role(), ReplRole::kReplica);
}

}  // namespace
}  // namespace tideskv::test
