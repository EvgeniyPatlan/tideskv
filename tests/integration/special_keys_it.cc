#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "support/full_sync.h"
#include "support/it_helpers.h"
#include "support/repl_pair.h"
#include "support/test_server.h"

// B1 end to end: the special-key filter of a real server is complete after a restart (the startup scan) and
// on a replica fed by streaming and by a full sync.
namespace tideskv::test {
namespace {

struct SpecialKeys {
  std::string state;
  int64_t specials = -1;
  int64_t missing = -1;
};

SpecialKeys DebugSpecialKeys(RespClient& c) {
  const RespValue r = c.Command({"DEBUG", "SPECIAL-KEYS"});
  EXPECT_EQ(r.elements.size(), 3u) << ToDebugString(r);
  if (r.elements.size() != 3) return {};
  return {r.elements[0].str, r.elements[1].integer, r.elements[2].integer};
}

bool FilterReady(RespClient& c) { return InfoString(c, "stats", "blind_set_filter_state") == "ready"; }

std::string Digest(RespClient& c) { return c.Command({"DEBUG", "DIGEST"}).str; }

// `rounds` x 9 commands that leave 7 special keys "<p>..." (a hash, set, zset, list, TTL string, a list
// moved into a new key, a renamed hash) and a plain string. Returns false on the first error reply.
bool FillMixed(RespClient& c, const std::string& p, int rounds) {
  for (int i = 0; i < rounds; ++i) {
    const std::string n = std::to_string(i);
    const std::vector<std::vector<std::string>> cmds = {
        {"HSET", p + "h" + n, "f", "v"},
        {"SADD", p + "s" + n, "a"},
        {"ZADD", p + "z" + n, "1", "a"},
        {"RPUSH", p + "l" + n, "a", "b"},
        {"SET", p + "t" + n, "v", "EX", "100000"},
        {"LMOVE", p + "l" + n, p + "m" + n, "LEFT", "RIGHT"},
        {"HSET", p + "rh" + n, "f", "v"},
        {"RENAME", p + "rh" + n, p + "renamed" + n},
        {"SET", p + "plain" + n, "v"},
    };
    for (const auto& cmd : cmds) {
      const RespValue r = c.Command(cmd);
      if (r.type == RespValue::Type::kError) return false;
    }
  }
  return true;
}

TEST(SpecialKeysIT, RestartScanFindsSpecials) {
  TestServer server{TIDESKV_SERVER_BIN};
  {
    auto c = server.Connect();
    for (int i = 0; i < 50; ++i) {
      const std::string n = std::to_string(i);
      ASSERT_EQ(c->Command({"HSET", "h" + n, "f", "v"}).integer, 1);
      ASSERT_EQ(c->Command({"ZADD", "z" + n, "1", "m"}).integer, 1);
      ASSERT_EQ(c->Command({"RPUSH", "l" + n, "a"}).integer, 1);
      ASSERT_EQ(c->Command({"SADD", "s" + n, "a"}).integer, 1);
      ASSERT_EQ(c->Command({"SET", "t" + n, "v", "EX", "100000"}).str, "OK");
    }
    for (int i = 0; i < 1000; ++i) ASSERT_EQ(c->Command({"SET", "p" + std::to_string(i), "v"}).str, "OK");
  }
  server.Restart();
  auto c = server.Connect();
  ASSERT_TRUE(WaitFor([&] { return FilterReady(*c); }, std::chrono::seconds(10)));
  const SpecialKeys k = DebugSpecialKeys(*c);
  EXPECT_EQ(k.state, "ready");
  EXPECT_EQ(k.specials, 250);
  EXPECT_EQ(k.missing, 0);
}

using SpecialKeysReplIT = ReplPairIT;

TEST_F(SpecialKeysReplIT, ReplicaKeepsFilterComplete) {
  auto m = master.Connect();
  auto r = replica.Connect();
  ASSERT_TRUE(FillMixed(*m, "a", 40));  // streamed
  ASSERT_TRUE(WaitFor([&] { return Digest(*r) == Digest(*m); }, std::chrono::seconds(10)));
  SpecialKeys mk = DebugSpecialKeys(*m);
  SpecialKeys rk = DebugSpecialKeys(*r);
  EXPECT_EQ(mk.missing, 0);
  EXPECT_EQ(mk.specials, 40 * 7);
  EXPECT_EQ(rk.missing, 0);
  EXPECT_EQ(rk.specials, mk.specials);

  // A full sync: the replica turns standalone, writes a key of its own and follows the master again.
  ASSERT_EQ(r->Command({"REPLICAOF", "NO", "ONE"}).str, "OK");
  ASSERT_TRUE(ForceFullSync(*r));
  ASSERT_EQ(r->Command({"HSET", "replica-only", "f", "v"}).integer, 1);
  const int64_t full_before = InfoField(*m, "stats", "sync_full");
  ASSERT_EQ(r->Command({"REPLICAOF", "127.0.0.1", std::to_string(master.port())}).str, "OK");
  ASSERT_TRUE(
      WaitFor([&] { return InfoField(*m, "stats", "sync_full") > full_before; }, std::chrono::seconds(10)));
  ASSERT_TRUE(WaitFor([&] { return InfoString(*r, "replication", "master_link_status") == "up"; },
                      std::chrono::seconds(10)));
  ASSERT_TRUE(FillMixed(*m, "b", 40));  // streamed after the load
  ASSERT_TRUE(WaitFor([&] { return Digest(*r) == Digest(*m); }, std::chrono::seconds(10)));
  // The load's end starts a rebuild; a filter that is still building may lack keys by design (SET reads).
  ASSERT_TRUE(WaitFor([&] { return FilterReady(*r); }, std::chrono::seconds(10)));
  mk = DebugSpecialKeys(*m);
  rk = DebugSpecialKeys(*r);
  EXPECT_EQ(mk.missing, 0);
  EXPECT_EQ(mk.specials, 80 * 7);
  EXPECT_EQ(rk.state, "ready");
  EXPECT_EQ(rk.missing, 0);
  EXPECT_EQ(rk.specials, mk.specials);
}

}  // namespace
}  // namespace tideskv::test
