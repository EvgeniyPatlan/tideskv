#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>

#include "script/sha1hex.h"
#include "support/command_harness.h"

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

TEST(PsyncCmd, FullResyncRepliesTheIdAndLastSeqThenCloses) {
  ReplHarness h;
  EXPECT_EQ(h.Run({"PSYNC", std::string(40, 'a'), "2"}),
            "+FULLRESYNC " + h.myid() + " " + std::to_string(h.last()) + "\r\n");
  EXPECT_EQ(h.last(), 3u);
  EXPECT_TRUE(h.session.close_after_reply);
  EXPECT_FALSE(h.session.takeover);
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
