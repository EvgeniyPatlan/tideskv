#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "repl/full_sync_frames.h"
#include "support/it_helpers.h"
#include "support/test_server.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

// The master side of replication end to end (M6a Task 11): PSYNC through the real server, the socket
// hand-off, the BATCH stream and a clean shutdown with a replica attached.
namespace tideskv::test {
namespace {

using Type = RespValue::Type;
using namespace std::chrono_literals;

struct ReplicationMasterIT : ::testing::Test {
  // An active log from the start: these cases CONTINUE over batches committed before any replica came
  // (repl-backlog-lazy would leave those unlogged; replication_lazy_it.cc covers that mode).
  TestServer s{TIDESKV_SERVER_BIN, {.extra_args = {"--repl-backlog-lazy", "no"}}};
  std::unique_ptr<RespClient> Client() {
    auto c = s.Connect();
    EXPECT_NE(c, nullptr);
    return c;
  }
  std::string MasterReplid() { return InfoString(*Client(), "replication", "master_replid"); }
};

// Reads frames until a BATCH arrives (PINGs may come first on an idle link).
std::optional<RespValue> NextBatch(RespClient& c) {
  for (int i = 0; i < 5; ++i) {
    auto f = c.Read();
    if (!f.has_value()) return f;
    if (!f->elements.empty() && f->elements[0].str == "BATCH") return f;
  }
  return std::nullopt;
}

TEST_F(ReplicationMasterIT, ContinueStreamsTheBacklogThenLiveBatches) {
  auto writer = Client();
  const std::string id = MasterReplid();
  ASSERT_EQ(id.size(), 40u);
  // Any seqs before these SETs (e.g. the oid lease) are skipped by asking from the current end.
  const auto last = static_cast<uint64_t>(InfoField(*Client(), "replication", "master_repl_offset"));
  EXPECT_EQ(writer->Command({"SET", "a", "1"}).str, "OK");
  EXPECT_EQ(writer->Command({"SET", "b", "2"}).str, "OK");

  auto replica = Client();
  ASSERT_TRUE(replica->Send({"REPLCONF", "listening-port", "7777"}));
  ASSERT_EQ(replica->Read()->str, "OK");
  ASSERT_EQ(DeclareSlotless(*replica), "slotless");
  // PSYNC and an ACK in one write: the ACK belongs to the stream and gets no reply.
  ASSERT_TRUE(replica->SendRaw(EncodeCommand({"PSYNC", id, std::to_string(last + 1)}) +
                               EncodeCommand({"REPLCONF", "ACK", std::to_string(last)})));
  auto cont = replica->Read();
  ASSERT_TRUE(cont.has_value());
  EXPECT_EQ(cont->str, "CONTINUE " + id);
  for (uint64_t seq = last + 1; seq <= last + 2; ++seq) {
    auto batch = NextBatch(*replica);
    ASSERT_TRUE(batch.has_value());
    ASSERT_EQ(batch->elements.size(), 3u);
    EXPECT_EQ(batch->elements[1].str, std::to_string(seq));
  }
  EXPECT_EQ(writer->Command({"SET", "c", "3"}).str, "OK");
  auto live = NextBatch(*replica);
  ASSERT_TRUE(live.has_value());
  EXPECT_EQ(live->elements[1].str, std::to_string(last + 3));
  // The replica link is no longer a client.
  EXPECT_TRUE(WaitFor([&] { return InfoField(*writer, "clients", "connected_clients") == 1; }));
  ASSERT_TRUE(replica->Send({"REPLCONF", "ACK", std::to_string(last + 3)}));
  EXPECT_EQ(s.Terminate(), 0);  // shuts down promptly with a replica attached
  EXPECT_TRUE(replica->WaitForClose());
}

// M6b D2, D4, D10: FULLRESYNC <id> <S>, the snapshot as LOAD frames, LOADEND S <replog[S]> <ops>, then
// the tail from S+1. The replica is listed by ROLE only once it acknowledged the load. (INFO's
// state=send_bulk/online is asserted with Task 10, which adds that field: PF2.)
TEST_F(ReplicationMasterIT, FullResyncStreamsTheSnapshotThenTheTail) {
  auto writer = Client();
  for (int i = 0; i < 10; ++i) ASSERT_EQ(writer->Command({"SET", "k" + std::to_string(i), "v"}).str, "OK");
  auto replica = Client();
  ASSERT_TRUE(replica->Send({"REPLCONF", "listening-port", "7778"}));
  ASSERT_EQ(replica->Read()->str, "OK");
  ASSERT_EQ(DeclareSlotless(*replica), "slotless");
  ASSERT_TRUE(replica->Send({"PSYNC", std::string(40, '0'), "1"}));
  const auto full = replica->Read();
  ASSERT_TRUE(full.has_value());
  ASSERT_EQ(full->str.rfind("FULLRESYNC ", 0), 0u) << full->str;
  const std::string seq = full->str.substr(full->str.rfind(' ') + 1);
  std::optional<RespValue> f = replica->Read();
  uint64_t loaded = 0;
  while (f.has_value() && f->elements.size() == 2 && f->elements[0].str == "LOAD") {
    auto ops = repl::DecodeLoadChunk(f->elements[1].str);
    ASSERT_TRUE(ops.ok());
    loaded += ops->size();
    f = replica->Read();
  }
  ASSERT_TRUE(f.has_value());
  ASSERT_EQ(f->elements.size(), 4u);
  EXPECT_EQ(f->elements[0].str, "LOADEND");
  EXPECT_EQ(f->elements[1].str, seq);
  EXPECT_EQ(f->elements[3].str, std::to_string(loaded));
  EXPECT_GE(loaded, 10u);                                                // k0..k9 (and sys/oid_hwm)
  EXPECT_EQ(writer->Command({"ROLE"}).elements[2].elements.size(), 0u);  // ROLE lists online replicas only
  ASSERT_TRUE(replica->Send({"REPLCONF", "ACK", seq}));
  EXPECT_TRUE(WaitFor([&] { return writer->Command({"ROLE"}).elements[2].elements.size() == 1u; }));
  ASSERT_EQ(writer->Command({"SET", "after", "1"}).str, "OK");
  auto batch = NextBatch(*replica);
  ASSERT_TRUE(batch.has_value());
  EXPECT_EQ(batch->elements[1].str, std::to_string(std::stoull(seq) + 1));
}

// F4: replica links count against maxclients like the clients they were: once the limit is reached,
// PSYNC gets the server's maxclients error before any hand-off, and a dropped link frees its slot.
TEST(ReplicationLimitIT, ReplicaLinksCountAgainstMaxclients) {
  TestServer s(TIDESKV_SERVER_BIN, {.extra_args = {"--maxclients", "2"}});
  std::vector<std::unique_ptr<RespClient>> links;
  for (int i = 0; i < 2; ++i) {
    // The server may still count the readiness probe's connection and refuse this one at accept.
    std::unique_ptr<RespClient> link;
    ASSERT_TRUE(WaitFor([&] {
      link = s.Connect();
      return link != nullptr && link->Command({"PING"}).str == "PONG";
    }));
    links.push_back(std::move(link));
  }
  // At the limit (2 connections): the first link is handed off, leaving 1 connection + 1 link, and
  // the second fits too.
  for (auto& link : links) {
    ASSERT_EQ(DeclareSlotless(*link), "slotless");
    const RespValue r = link->Command({"PSYNC", "?", "1"});
    ASSERT_EQ(r.str.rfind("CONTINUE ", 0), 0u) << r.str;
    if (&link == &links.front()) {
      ASSERT_TRUE(WaitFor([&] { return InfoField(*links.back(), "clients", "connected_clients") == 1; }));
    }
  }
  std::unique_ptr<RespClient> c;
  ASSERT_TRUE(WaitFor([&] {  // both hand-offs done: their connections are gone
    c = s.Connect();
    return c != nullptr && InfoField(*c, "clients", "connected_clients") == 1;
  }));
  ASSERT_EQ(DeclareSlotless(*c), "slotless");
  const RespValue refused = c->Command({"PSYNC", "?", "1"});
  EXPECT_EQ(refused.type, Type::kError);
  EXPECT_EQ(refused.str, "ERR max number of clients reached");
  EXPECT_EQ(InfoField(*c, "stats", "sync_partial_ok"), 2);
  links.pop_back();
  EXPECT_TRUE(WaitFor([&] { return c->Command({"PSYNC", "?", "1"}).str.rfind("CONTINUE ", 0) == 0; }));
}

}  // namespace
}  // namespace tideskv::test
