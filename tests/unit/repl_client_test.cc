#include "repl/repl_client.h"

#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "common/string_util.h"
#include "repl/backlog_trimmer.h"
#include "repl/repl_sender.h"
#include "repl/repl_state.h"
#include "repl/socket_io.h"
#include "resp/request_parser.h"
#include "storage/memory_storage.h"
#include "support/command_harness.h"
#include "support/faulty_storage.h"
#include "support/replica_stack.h"
#include "support/resp_client.h"

namespace tideskv::repl {
namespace {

using namespace std::chrono_literals;
using Args = std::vector<std::string>;

bool WaitFor(const std::function<bool()>& cond, std::chrono::milliseconds timeout = 3s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (cond()) return true;
    std::this_thread::sleep_for(5ms);
  }
  return cond();
}

// A listening socket on 127.0.0.1 standing in for the master.
class FakeMaster {
 public:
  explicit FakeMaster(int backlog = 16) {
    fd_ = Fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    EXPECT_EQ(::bind(fd_.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    EXPECT_EQ(::listen(fd_.get(), backlog), 0);
    EXPECT_EQ(::getsockname(fd_.get(), reinterpret_cast<sockaddr*>(&addr), &len), 0);
    port_ = ntohs(addr.sin_port);
  }
  uint16_t port() const { return port_; }
  // The next connection, or an invalid Fd if none arrives in time.
  Fd Accept(std::chrono::milliseconds timeout = 5s) {
    pollfd p{fd_.get(), POLLIN, 0};
    if (::poll(&p, 1, static_cast<int>(timeout.count())) != 1) return Fd();
    return Fd(::accept4(fd_.get(), nullptr, nullptr, SOCK_CLOEXEC));
  }

 private:
  Fd fd_;
  uint16_t port_ = 0;
};

// The master's end of one accepted link: reads the replica's commands, writes replies and frames.
class Link {
 public:
  explicit Link(Fd fd) : fd_(std::move(fd)) {}
  bool valid() const { return fd_.get() >= 0; }
  std::optional<Args> Next(std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
      size_t consumed = 0;
      const auto ps = parser_.Parse(in_, &consumed);
      in_.erase(0, consumed);
      if (ps == resp::ParseStatus::kCommand) return parser_.args();
      if (ps == resp::ParseStatus::kError || std::chrono::steady_clock::now() >= deadline)
        return std::nullopt;
      const Status st = ReadSome(fd_.get(), &in_, 20ms);
      if (!st.ok() && st.code() != StatusCode::kTimedOut) return std::nullopt;
    }
  }
  void Send(std::string_view bytes) { EXPECT_TRUE(WriteAll(fd_.get(), bytes, 2s).ok()); }
  // True once the replica closed its end.
  bool Closed(std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      const Status st = ReadSome(fd_.get(), &in_, 20ms);
      if (st.code() == StatusCode::kIOError) return true;
    }
    return false;
  }

 private:
  Fd fd_;
  std::string in_;
  resp::RequestParser parser_;
};

struct ReplClientTest : ::testing::Test {
  ReplClientTest() {
    cfg.port = 7001;
    identity.replid = std::string(kReplIdLength, 'a');
  }
  ~ReplClientTest() override {
    if (client) client->Stop();
  }

  void Write(std::initializer_list<std::string_view> argv) {
    ASSERT_FALSE(master.Run(argv).starts_with("-"));
  }
  uint64_t master_last() { return master.services.repl_log_.last(); }
  const std::string& master_id() { return master.services.replication_.identity.replid; }
  std::string Payload(uint64_t seq) {
    auto p = test::ReplogEntry(master.services.storage_, seq);
    EXPECT_TRUE(p.ok());
    return p.ok() ? *p : std::string();
  }
  ReplIdentity Identity() {
    std::lock_guard lock(mu);
    return identity;
  }

  void StartClient(ReplClient::Options o = Fast()) {
    ReplClient::Callbacks cb;
    cb.identity = [this] { return Identity(); };
    cb.persist_identity = [this](const ReplIdentity& id) {
      if (Status st = PersistIdentity(replica_storage, id); !st.ok()) return st;
      std::lock_guard lock(mu);
      identity = id;
      return Status::Ok();
    };
    cb.on_adopt = [this](const std::string& id) {
      std::lock_guard lock(mu);
      adopted.push_back(id);
    };
    cb.trim_backlog = trim_hook;
    cb.on_state = [this](ReplicaLinkState s) {
      std::lock_guard lock(mu);
      states.push_back(s);
    };
    auto config = [this] {
      std::lock_guard lock(mu);
      return cfg;
    };
    client = std::make_unique<ReplClient>(replica.engine, replica.log, config, cb, o);
    client->Start("127.0.0.1", fake.port());
  }
  static ReplClient::Options Fast() {
    ReplClient::Options o;
    o.backoff_initial = 50ms;
    o.backoff_max = 200ms;
    return o;
  }

  Link AcceptLink() {
    Link l(fake.Accept());
    EXPECT_TRUE(l.valid()) << "the client did not connect";
    return l;
  }
  // Answers the handshake (+PONG to PING, +OK to the rest) up to PSYNC, which gets `psync_reply`.
  std::vector<Args> Handshake(Link& l, const std::string& psync_reply) {
    std::vector<Args> seen;
    for (int i = 0; i < 10; ++i) {
      auto cmd = l.Next();
      if (!cmd) break;
      seen.push_back(*cmd);
      const std::string name = ToLower((*cmd)[0]);
      if (name == "psync") {
        l.Send("+" + psync_reply + "\r\n");
        break;
      }
      l.Send(name == "ping" ? "+PONG\r\n" : "+OK\r\n");
    }
    return seen;
  }
  void SendBatches(Link& l, uint64_t from, uint64_t to) {
    std::string out;
    for (uint64_t seq = from; seq <= to; ++seq) out += BatchFrame(seq, Payload(seq));
    l.Send(out);
  }
  // Reads frames until REPLCONF ACK <seq> (or a later one) arrives.
  bool WaitAck(Link& l, uint64_t seq, std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      auto f = l.Next(100ms);
      if (!f) continue;
      EXPECT_EQ(f->size(), 3u);  // CF12: exactly REPLCONF ACK <seq>
      if (f->size() == 3 && (*f)[0] == "REPLCONF" && (*f)[1] == "ACK" && std::stoull((*f)[2]) >= seq)
        return true;
    }
    return false;
  }
  bool SawState(ReplicaLinkState s) {
    std::lock_guard lock(mu);
    return std::find(states.begin(), states.end(), s) != states.end();
  }

  test::CommandHarness master;
  test::FaultyStorage replica_storage;  // counts the write txns alive at once
  test::ReplicaStack replica{replica_storage};
  std::function<Status()> trim_hook;
  FakeMaster fake;
  std::mutex mu;
  ConfigValues cfg;
  ReplIdentity identity;
  std::vector<std::string> adopted;
  std::vector<ReplicaLinkState> states;
  std::unique_ptr<ReplClient> client;
};

TEST_F(ReplClientTest, HandshakeSendsTheExpectedCommands) {
  StartClient();
  Link l = AcceptLink();
  const auto seen = Handshake(l, "FULLRESYNC " + master_id() + " 0");
  EXPECT_EQ(seen, (std::vector<Args>{{"PING"},
                                     {"REPLCONF", "listening-port", "7001"},
                                     {"REPLCONF", "capa", "psync2"},
                                     {"PSYNC", "?", "1"}}));
  client->Stop();
  {
    std::lock_guard lock(mu);
    cfg.masterauth = "secret";
    cfg.replica_announce_ip = "10.0.0.7";
    cfg.replica_announce_port = 7100;
  }
  client.reset();
  StartClient();
  Link l2 = AcceptLink();
  EXPECT_EQ(Handshake(l2, "FULLRESYNC " + master_id() + " 0"),
            (std::vector<Args>{{"PING"},
                               {"AUTH", "secret"},
                               {"REPLCONF", "listening-port", "7100"},
                               {"REPLCONF", "ip-address", "10.0.0.7"},
                               {"REPLCONF", "capa", "psync2"},
                               {"PSYNC", "?", "1"}}));
  client.reset();
  {
    std::lock_guard lock(mu);
    cfg.masteruser = "repl";
  }
  StartClient();
  Link l3 = AcceptLink();
  const auto third = Handshake(l3, "FULLRESYNC " + master_id() + " 0");
  ASSERT_GE(third.size(), 2u);
  EXPECT_EQ(third[1], (Args{"AUTH", "repl", "secret"}));
}

// F5: a replica whose own baseline is unknown never asks with "?" (which an empty log would otherwise
// send); it names its own replid, which no master continues from seq 1.
TEST_F(ReplClientTest, AnUnknownBaselineNamesItsOwnReplidInsteadOfQuestionMark) {
  ReplClient::Options o = Fast();
  o.baseline_unknown = true;
  StartClient(o);
  Link l = AcceptLink();
  const auto seen = Handshake(l, "FULLRESYNC " + master_id() + " 0");
  ASSERT_FALSE(seen.empty());
  EXPECT_EQ(seen.back(), (Args{"PSYNC", Identity().replid, "1"}));
  EXPECT_EQ(replica.log.last(), 0u);
}

TEST_F(ReplClientTest, ARefusedAuthGoesToBackoff) {
  {
    std::lock_guard lock(mu);
    cfg.masterauth = "wrong";
  }
  StartClient();
  Link l = AcceptLink();
  ASSERT_EQ(l.Next(), (Args{"PING"}));
  l.Send("-NOAUTH Authentication required.\r\n");  // PING before AUTH: accepted
  ASSERT_EQ(l.Next(), (Args{"AUTH", "wrong"}));
  l.Send("-WRONGPASS invalid username-password pair or user is disabled.\r\n");
  EXPECT_TRUE(l.Closed());
  EXPECT_FALSE(SawState(ReplicaLinkState::kConnected));
  Link again = AcceptLink();  // retried after the backoff
  EXPECT_EQ(again.Next(), (Args{"PING"}));
}

TEST_F(ReplClientTest, ContinueThenAppliesBatchesAndAcks) {
  for (int i = 0; i < 10; ++i) Write({"SET", "k" + std::to_string(i), "v" + std::to_string(i)});
  ASSERT_EQ(master_last(), 10u);
  StartClient();
  Link l = AcceptLink();
  Handshake(l, "CONTINUE " + master_id());
  EXPECT_TRUE(WaitFor([&] { return client->state() == ReplicaLinkState::kConnected; }));
  EXPECT_EQ(client->link_down_since_ms(), 0);
  SendBatches(l, 1, 10);
  EXPECT_TRUE(WaitAck(l, 10));
  EXPECT_EQ(replica.log.last(), 10u);
  EXPECT_EQ(test::DigestOf(replica.engine), test::DigestOf(master.services.engine_));
  EXPECT_GT(client->last_io_ms(), 0);
  l.Send(kPingFrame);  // a PING is answered with an ACK
  EXPECT_TRUE(WaitAck(l, 10, 500ms));
}

TEST_F(ReplClientTest, AdoptsANewReplidAndPushesHistory) {
  for (int i = 0; i < 3; ++i) Write({"SET", "k" + std::to_string(i), "v"});
  ASSERT_TRUE(test::ShipAll(master.services.storage_, 3, replica.engine).ok());
  const std::string own = Identity().replid;
  StartClient();
  Link l = AcceptLink();
  const auto seen = Handshake(l, "CONTINUE " + master_id());
  ASSERT_FALSE(seen.empty());
  EXPECT_EQ(seen.back(), (Args{"PSYNC", own, "4"}));
  ASSERT_TRUE(WaitFor([&] { return Identity().replid == master_id(); }));
  const ReplIdentity now = Identity();
  ASSERT_EQ(now.history.size(), 1u);
  EXPECT_EQ(now.history[0], (HistoryEntry{own, 3}));
  auto persisted = LoadOrCreateIdentity(replica_storage);
  ASSERT_TRUE(persisted.ok());
  EXPECT_EQ(persisted->replid, master_id());
  EXPECT_EQ(persisted->history, now.history);
  std::lock_guard lock(mu);
  EXPECT_EQ(adopted, (std::vector<std::string>{master_id()}));
}

TEST_F(ReplClientTest, AnInvalidMasterReplidIsAProtocolError) {  // CF5
  for (const std::string& reply :
       std::vector<std::string>{"CONTINUE not-a-replid", "CONTINUE " + std::string(40, 'G'),
                                "FULLRESYNC " + std::string(39, 'b') + " 7", "WHATEVER"}) {
    StartClient();
    Link l = AcceptLink();
    Handshake(l, reply);
    EXPECT_TRUE(l.Closed()) << reply;
    EXPECT_EQ(Identity().replid, std::string(kReplIdLength, 'a')) << reply;
    EXPECT_FALSE(SawState(ReplicaLinkState::kConnected)) << reply;
    client.reset();
  }
}

TEST_F(ReplClientTest, FullResyncGoesToBackoffWithoutTouchingData) {
  for (int i = 0; i < 3; ++i) Write({"SET", "k" + std::to_string(i), "v"});
  ASSERT_TRUE(test::ShipAll(master.services.storage_, 3, replica.engine).ok());
  const std::string digest = test::DigestOf(replica.engine);
  StartClient();
  Link l = AcceptLink();
  Handshake(l, "FULLRESYNC " + std::string(kReplIdLength, 'd') + " 99");
  EXPECT_TRUE(l.Closed());
  EXPECT_TRUE(WaitFor([&] { return client->state() != ReplicaLinkState::kHandshake; }));
  EXPECT_FALSE(SawState(ReplicaLinkState::kConnected));
  EXPECT_EQ(client->link_down_since_ms(), -1);  // never connected
  EXPECT_EQ(replica.log.last(), 3u);
  EXPECT_EQ(test::DigestOf(replica.engine), digest);
  EXPECT_EQ(Identity().replid, std::string(kReplIdLength, 'a'));
  Link again = AcceptLink();  // it retries after the backoff
  EXPECT_EQ(again.Next(), (Args{"PING"}));
}

TEST_F(ReplClientTest, ARefusedPsyncGoesToBackoff) {
  StartClient();
  Link l = AcceptLink();
  Handshake(l, "");  // an empty status line is not a PSYNC answer either
  EXPECT_TRUE(l.Closed());
  Link l2 = AcceptLink();
  ASSERT_EQ(l2.Next(), (Args{"PING"}));
  l2.Send("+PONG\r\n");
  for (int i = 0; i < 2; ++i) {
    ASSERT_TRUE(l2.Next().has_value());
    l2.Send("+OK\r\n");
  }
  ASSERT_EQ(l2.Next(), (Args{"PSYNC", "?", "1"}));
  l2.Send("-LOADING Redis is loading the dataset in memory\r\n");
  EXPECT_TRUE(l2.Closed());
  EXPECT_FALSE(SawState(ReplicaLinkState::kConnected));
}

TEST_F(ReplClientTest, GapDropsTheLinkAndReconnectsFromLastApplied) {
  for (int i = 0; i < 4; ++i) Write({"SET", "k" + std::to_string(i), "v"});
  StartClient();
  Link l = AcceptLink();
  Handshake(l, "CONTINUE " + master_id());
  SendBatches(l, 1, 2);
  ASSERT_TRUE(WaitFor([&] { return replica.log.last() == 2; }));
  l.Send(BatchFrame(4, Payload(4)));  // seq 3 is missing
  EXPECT_TRUE(l.Closed());
  EXPECT_EQ(replica.log.last(), 2u);
  EXPECT_TRUE(WaitFor([&] { return client->link_down_since_ms() > 0; }));
  Link again = AcceptLink();
  const auto seen = Handshake(again, "CONTINUE " + master_id());
  ASSERT_FALSE(seen.empty());
  EXPECT_EQ(seen.back(), (Args{"PSYNC", master_id(), "3"}));
  SendBatches(again, 3, 4);
  EXPECT_TRUE(WaitAck(again, 4));
  EXPECT_EQ(test::DigestOf(replica.engine), test::DigestOf(master.services.engine_));
}

TEST_F(ReplClientTest, ABatchThatDoesNotApplyDropsTheLink) {
  Write({"SET", "a", "1"});
  StartClient();
  Link l = AcceptLink();
  Handshake(l, "CONTINUE " + master_id());
  std::string bad = Payload(1);
  bad[bad.size() / 2] ^= 0x01;
  l.Send(BatchFrame(1, bad));
  EXPECT_TRUE(l.Closed());
  EXPECT_EQ(replica.log.last(), 0u);
  Link again = AcceptLink();
  const auto seen = Handshake(again, "CONTINUE " + master_id());
  ASSERT_FALSE(seen.empty());
  EXPECT_EQ(seen.back(), (Args{"PSYNC", "?", "1"}));  // D8: "?" while nothing is applied
}

TEST_F(ReplClientTest, AnUnexpectedFrameDropsTheLink) {
  StartClient();
  Link l = AcceptLink();
  Handshake(l, "CONTINUE " + master_id());
  l.Send(test::EncodeCommand({"SET", "x", "y"}));
  EXPECT_TRUE(l.Closed());
}

TEST_F(ReplClientTest, AnOversizedBatchFrameDropsTheLink) {  // Task 3 hardening: bounded input
  StartClient();
  Link l = AcceptLink();
  Handshake(l, "CONTINUE " + master_id());
  l.Send("*3\r\n$5\r\nBATCH\r\n$1\r\n1\r\n$" + std::to_string(encoding::kMaxReplBatchBytes + 1) + "\r\n");
  EXPECT_TRUE(l.Closed());
  EXPECT_EQ(replica.log.last(), 0u);
}

TEST_F(ReplClientTest, SilentMasterTimesOut) {
  {
    std::lock_guard lock(mu);
    cfg.repl_timeout = 1;
  }
  StartClient();
  Link l = AcceptLink();
  Handshake(l, "CONTINUE " + master_id());
  ASSERT_TRUE(WaitFor([&] { return client->state() == ReplicaLinkState::kConnected; }));
  const auto start = std::chrono::steady_clock::now();
  EXPECT_TRUE(l.Closed(4s));
  const auto waited = std::chrono::steady_clock::now() - start;
  EXPECT_GE(waited, 800ms);
  EXPECT_LT(waited, 3s);
  Link again = AcceptLink();
  EXPECT_EQ(again.Next(), (Args{"PING"}));
}

TEST_F(ReplClientTest, AnIdleLinkKeptAliveByPingsStaysUp) {
  {
    std::lock_guard lock(mu);
    cfg.repl_timeout = 1;
  }
  StartClient();
  Link l = AcceptLink();
  Handshake(l, "CONTINUE " + master_id());
  for (int i = 0; i < 8; ++i) {
    std::this_thread::sleep_for(250ms);
    l.Send(kPingFrame);
  }
  EXPECT_EQ(client->state(), ReplicaLinkState::kConnected);
}

TEST_F(ReplClientTest, StopIsPromptDuringHandshakeAndStreaming) {
  StartClient();
  Link l = AcceptLink();
  ASSERT_EQ(l.Next(), (Args{"PING"}));  // never answered: the client waits up to repl-timeout (60 s)
  auto start = std::chrono::steady_clock::now();
  client->Stop();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
  client.reset();
  StartClient();
  Link l2 = AcceptLink();
  Handshake(l2, "CONTINUE " + master_id());
  ASSERT_TRUE(WaitFor([&] { return client->state() == ReplicaLinkState::kConnected; }));
  start = std::chrono::steady_clock::now();
  client->Stop();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
  EXPECT_TRUE(l2.Closed());
}

TEST_F(ReplClientTest, StopIsPromptDuringConnectAndBackoff) {
  // A full accept queue drops further SYNs, so the client's connect hangs until Stop cancels it.
  FakeMaster full(0);
  std::vector<Fd> fillers;
  for (int i = 0; i < 4; ++i) {
    auto f = ConnectTcp("127.0.0.1", full.port(), 100ms);
    if (f.ok()) fillers.push_back(std::move(f).value());
  }
  ReplClient::Callbacks cb;
  cb.identity = [this] { return Identity(); };
  cb.persist_identity = [](const ReplIdentity&) { return Status::Ok(); };
  ReplClient connecting(replica.engine, replica.log, [this] { return cfg; }, cb);
  connecting.Start("127.0.0.1", full.port());
  WaitFor([&] { return connecting.state() == ReplicaLinkState::kConnecting; }, 1s);
  std::this_thread::sleep_for(100ms);
  auto start = std::chrono::steady_clock::now();
  connecting.Stop();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);

  ReplClient::Options slow;
  slow.backoff_initial = 30s;
  StartClient(slow);
  Link l = AcceptLink();
  Handshake(l, "FULLRESYNC " + master_id() + " 0");  // now waiting 30 s before retrying
  ASSERT_TRUE(l.Closed());
  start = std::chrono::steady_clock::now();
  client->Stop();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
}

// D18 / CF10: a replica trims its log on the applier thread, between batches, never during an apply
// (Engine::ApplyReplicated does not retry kConflict).
TEST_F(ReplClientTest, ReplicaTrimsBetweenBatchesWithoutConcurrentCommits) {
  for (int i = 0; i < 1000; ++i) Write({"SET", "k" + std::to_string(i % 50), std::to_string(i)});
  ASSERT_EQ(master_last(), 1000u);
  {
    std::lock_guard lock(mu);
    cfg.repl_backlog_size = 2000;  // a few dozen batches
  }
  BacklogTrimmer::Hooks hooks;
  hooks.config = [this] {
    std::lock_guard lock(mu);
    return cfg;
  };
  BacklogTrimmer trimmer(replica_storage, replica.log, nullptr, replica.clock, hooks, {});
  std::atomic<int> trims{0};
  std::atomic<int> during_apply{0};  // the test hook counter: trims that saw another write txn open
  trim_hook = [&] {
    if (replica_storage.open_txns.load() != 0) ++during_apply;
    ++trims;
    return trimmer.RunOnReplica();
  };
  ReplClient::Options o = Fast();
  o.trim_interval = 0ms;  // every chance it gets
  StartClient(o);
  Link l = AcceptLink();
  Handshake(l, "CONTINUE " + master_id());
  for (uint64_t from = 1; from <= 1000; from += 100) SendBatches(l, from, from + 99);
  ASSERT_TRUE(WaitAck(l, 1000, 10s));
  EXPECT_EQ(replica.log.last(), 1000u);
  EXPECT_GT(trims.load(), 1);
  EXPECT_EQ(during_apply.load(), 0);
  EXPECT_EQ(replica_storage.max_open_txns.load(), 1);  // no two write txns were ever open at once
  EXPECT_GT(replica.log.first_retained(), 900u);
  EXPECT_GT(trimmer.trimmed_total(), 900u);
  EXPECT_EQ(test::DigestOf(replica.engine), test::DigestOf(master.services.engine_));
}

TEST(ReplicaLinkStateNames, MatchRole) {
  EXPECT_EQ(LinkStateName(ReplicaLinkState::kConnect), "connect");
  EXPECT_EQ(LinkStateName(ReplicaLinkState::kConnecting), "connecting");
  EXPECT_EQ(LinkStateName(ReplicaLinkState::kHandshake), "handshake");
  EXPECT_EQ(LinkStateName(ReplicaLinkState::kConnected), "connected");
}

}  // namespace
}  // namespace tideskv::repl
