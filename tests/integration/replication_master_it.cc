#include <gtest/gtest.h>

#include <chrono>
#include <string>

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
  TestServer s{TIDESKV_SERVER_BIN};
  std::unique_ptr<RespClient> Client() {
    auto c = s.Connect();
    EXPECT_NE(c, nullptr);
    return c;
  }
  // The master's replid, from a FULLRESYNC (which closes that link in M6a).
  std::string MasterReplid() {
    auto c = Client();
    const RespValue r = c->Command({"PSYNC", "?", "-1"});
    EXPECT_EQ(r.type, Type::kSimple);
    EXPECT_TRUE(c->WaitForClose());
    return r.str.substr(std::string("FULLRESYNC ").size(), 40);
  }
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
  const RespValue probe =
      Client()->Command({"PSYNC", id, "999999"});  // beyond last+1: FULLRESYNC <id> <last>
  const uint64_t last = std::stoull(probe.str.substr(probe.str.rfind(' ') + 1));
  EXPECT_EQ(writer->Command({"SET", "a", "1"}).str, "OK");
  EXPECT_EQ(writer->Command({"SET", "b", "2"}).str, "OK");

  auto replica = Client();
  ASSERT_TRUE(replica->Send({"REPLCONF", "listening-port", "7777"}));
  ASSERT_EQ(replica->Read()->str, "OK");
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

TEST_F(ReplicationMasterIT, FullResyncClosesTheLink) {
  auto c = Client();
  const RespValue r = c->Command({"PSYNC", std::string(40, '0'), "1"});
  EXPECT_EQ(r.type, Type::kSimple);
  EXPECT_EQ(r.str.rfind("FULLRESYNC ", 0), 0u);
  EXPECT_TRUE(c->WaitForClose());
  EXPECT_EQ(Client()->Command({"PING"}).str, "PONG");
}

}  // namespace
}  // namespace tideskv::test
