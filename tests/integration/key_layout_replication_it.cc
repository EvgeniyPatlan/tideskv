#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <tuple>
#include <vector>

#include "common/crc16.h"
#include "encoding/meta_codec.h"
#include "storage/tides_storage.h"
#include "support/it_helpers.h"
#include "support/temp_dir.h"
#include "support/test_server.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

// Replication between the two key layouts (slotless key layout design, decision 1): a replica adopts its
// master's layout on the full resynchronisation, except a cluster replica, which refuses a slotless master
// before PSYNC.
namespace tideskv::test {
namespace {

using namespace std::chrono_literals;

std::unique_ptr<TidesStorage> OpenStore(const std::string& dir) {
  StorageOptions o;
  o.path = dir + "/db";
  o.sync_mode = SyncMode::kNone;
  auto s = TidesStorage::Open(o);
  EXPECT_TRUE(s.ok()) << s.status().ToString();
  return s.ok() ? std::move(s).value() : nullptr;
}

std::string FormatVersionOf(const std::string& dir) {
  auto s = OpenStore(dir);
  if (s == nullptr) return "<cannot open>";
  auto view = s->BeginRead();
  auto v = (*view)->Get(ColumnFamily::kSys, "format_version");
  return v.ok() ? *v : "<" + v.status().ToString() + ">";
}

// A data dir an older TidesKV left: format 1, the string `key` = `value` in db 0 with its slot.
void SeedSlottedDir(const TempDir& dir, const std::string& key, const std::string& value) {
  auto s = OpenStore(dir.str());
  ASSERT_NE(s, nullptr);
  encoding::Metadata m;
  m.type = encoding::ValueType::kString;
  std::string meta_key(1, '\0');
  meta_key += static_cast<char>(KeySlot(key) >> 8);
  meta_key += static_cast<char>(KeySlot(key) & 0xff);
  meta_key += key;
  auto txn = s->BeginWrite();
  ASSERT_TRUE((*txn)->Put(ColumnFamily::kSys, "format_version", "1").ok());
  ASSERT_TRUE((*txn)->Put(ColumnFamily::kMeta, meta_key, encoding::EncodeMetadata(m, value)).ok());
  ASSERT_TRUE((*txn)->Commit().ok());
}

void WriteData(RespClient& c) {
  ASSERT_EQ(c.Command({"HSET", "h", "f1", "v1", "f2", "v2"}).integer, 2);
  ASSERT_EQ(c.Command({"ZADD", "z", "1", "a", "2", "b"}).integer, 2);
  ASSERT_EQ(c.Command({"RPUSH", "l", "x", "y", "z"}).integer, 3);
  ASSERT_EQ(c.Command({"SADD", "s", "m1", "m2"}).integer, 2);
  ASSERT_EQ(c.Command({"SET", "ttl", "v", "EX", "1000"}).str, "OK");
}

std::string Digest(RespClient& c) { return c.Command({"DEBUG", "DIGEST"}).str; }
std::string Link(RespClient& c) { return InfoString(c, "replication", "master_link_status"); }

void Follow(RespClient& replica, const TestServer& master) {
  ASSERT_EQ(replica.Command({"REPLICAOF", "127.0.0.1", std::to_string(master.port())}).str, "OK");
}

TEST(KeyLayoutReplicationIT, ASlotlessReplicaAdoptsASlottedMaster) {
  TempDir master_dir("tideskv-kl-master");
  SeedSlottedDir(master_dir, "legacy", "old value");
  TestServer m(TIDESKV_SERVER_BIN, {.dir = master_dir.str()});
  TestServer r(TIDESKV_SERVER_BIN);  // a fresh dir: slotless
  auto mc = m.Connect();
  auto rc = r.Connect();
  WriteData(*mc);
  ASSERT_EQ(rc->Command({"SET", "replica-only", "v"}).str, "OK");
  Follow(*rc, m);
  ASSERT_TRUE(WaitFor([&] { return Link(*rc) == "up" && Digest(*rc) == Digest(*mc); }, 20s)) << r.Stderr();
  EXPECT_EQ(rc->Command({"GET", "legacy"}).str, "old value");
  EXPECT_EQ(rc->Command({"EXISTS", "replica-only"}).integer, 0);
  EXPECT_NE(r.Stderr().find("now uses the master's slotted key layout (format 1)"), std::string::npos);
  ASSERT_EQ(mc->Command({"HSET", "h", "f3", "v3"}).integer, 1);  // streaming continues in the adopted layout
  ASSERT_TRUE(WaitFor([&] { return Digest(*rc) == Digest(*mc); }, 10s));
  const std::string digest = Digest(*mc);
  rc.reset();
  ASSERT_EQ(r.Terminate(), 0);
  EXPECT_EQ(FormatVersionOf(r.dir()), "1");
  r.Restart();  // the replica reopens its dir in the adopted layout
  rc = r.Connect();
  EXPECT_EQ(rc->Command({"HGET", "h", "f3"}).str, "v3");
  EXPECT_EQ(Digest(*rc), digest);
}

TEST(KeyLayoutReplicationIT, ASlottedReplicaAdoptsASlotlessMaster) {
  TestServer m(TIDESKV_SERVER_BIN);  // a fresh dir: slotless
  TempDir replica_dir("tideskv-kl-replica");
  SeedSlottedDir(replica_dir, "stale", "replaced by the full sync");
  TestServer r(TIDESKV_SERVER_BIN, {.dir = replica_dir.str()});
  auto mc = m.Connect();
  auto rc = r.Connect();
  ASSERT_EQ(rc->Command({"GET", "stale"}).str, "replaced by the full sync");
  WriteData(*mc);
  Follow(*rc, m);
  ASSERT_TRUE(WaitFor([&] { return Link(*rc) == "up" && Digest(*rc) == Digest(*mc); }, 20s)) << r.Stderr();
  EXPECT_EQ(rc->Command({"EXISTS", "stale"}).integer, 0);
  EXPECT_EQ(rc->Command({"ZSCORE", "z", "b"}).str, "2");
  rc.reset();
  ASSERT_EQ(r.Terminate(), 0);
  EXPECT_EQ(FormatVersionOf(replica_dir.str()), "2");
}

TEST(KeyLayoutReplicationIT, AClusterReplicaRefusesASlotlessMasterBeforePsync) {
  TestServer m(TIDESKV_SERVER_BIN);  // standalone, slotless
  auto mc = m.Connect();
  WriteData(*mc);
  TestServer r(TIDESKV_SERVER_BIN, {.extra_args = {"--cluster-enabled", "yes"}});
  auto rc = r.Connect();
  const std::string id_a(40, 'a');
  const std::string id_b(40, 'b');
  const std::string nodes = id_a + " 127.0.0.1 " + std::to_string(m.port()) + " master - 0-16383\n" + id_b +
                            " 127.0.0.1 " + std::to_string(r.port()) + " slave " + id_a + "\n";
  ASSERT_EQ(rc->Command({"CLUSTERX", "SETNODEID", id_b}).str, "OK");
  ASSERT_EQ(rc->Command({"CLUSTERX", "SETNODES", nodes, "1"}).str, "OK");
  ASSERT_EQ(InfoString(*rc, "replication", "role"), "slave");
  ASSERT_TRUE(WaitFor(
      [&] { return r.Stderr().find("cluster mode needs the slotted layout") != std::string::npos; }, 10s))
      << r.Stderr();
  EXPECT_EQ(Link(*rc), "down");
  EXPECT_EQ(InfoField(*mc, "stats", "sync_full"), 0);  // no PSYNC reached the master: nothing was wiped
  EXPECT_EQ(InfoField(*rc, "persistence", "loading"), 0);
  EXPECT_EQ(Digest(*rc), std::string(40, '0'));
  rc.reset();
  ASSERT_EQ(r.Terminate(), 0);
  EXPECT_EQ(FormatVersionOf(r.dir()), "1");
}

TEST(KeyLayoutReplicationIT, SameLayoutReplicasContinueAfterARestart) {  // the layout never forces a resync
  TestServer m(TIDESKV_SERVER_BIN);
  TestServer r(TIDESKV_SERVER_BIN);
  auto mc = m.Connect();
  auto rc = r.Connect();
  WriteData(*mc);
  Follow(*rc, m);
  ASSERT_TRUE(WaitFor([&] { return Link(*rc) == "up" && Digest(*rc) == Digest(*mc); }, 20s));
  const int64_t full = InfoField(*mc, "stats", "sync_full");
  rc.reset();
  ASSERT_EQ(r.Terminate(), 0);
  r.Restart({"--replicaof", "127.0.0.1 " + std::to_string(m.port())});  // keeps the master's replid
  rc = r.Connect();
  ASSERT_EQ(mc->Command({"SET", "after", "1"}).str, "OK");
  ASSERT_TRUE(WaitFor([&] { return Link(*rc) == "up" && Digest(*rc) == Digest(*mc); }, 20s));
  EXPECT_EQ(InfoField(*mc, "stats", "sync_full"), full);
  EXPECT_GE(InfoField(*mc, "stats", "sync_partial_ok"), 1);
}

}  // namespace
}  // namespace tideskv::test
