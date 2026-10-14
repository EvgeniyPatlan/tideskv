#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

#include "encoding/replog_codec.h"
#include "storage/tides_storage.h"
#include "support/it_helpers.h"
#include "support/temp_dir.h"
#include "support/test_server.h"

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

// F5: a data directory written before M6a has keys but no replication log. Its log does not describe its
// data, so it never takes part in a partial resynchronisation from seq 1, as a master or as a replica.
namespace tideskv::test {
namespace {

using Type = RespValue::Type;
using namespace std::chrono_literals;

constexpr const char* kFullResyncLog = "is required; it is implemented in M6b";
constexpr const char* kBaselineWarning = "predates the replication log";

// Fills `dir` through a real server, then strips what M6a added (the replog and the replication keys in
// sys), leaving what an M1-M5 server would have left: the same keys and no log.
void MakePreReplicationDir(const TempDir& dir) {
  {
    TestServer s(TIDESKV_SERVER_BIN, {.dir = dir.str()});
    auto c = s.Connect();
    ASSERT_NE(c, nullptr);
    ASSERT_EQ(c->Command({"SET", "old", "v"}).str, "OK");
    ASSERT_EQ(c->Command({"HSET", "oldhash", "f", "v"}).integer, 1);
    ASSERT_EQ(s.Terminate(), 0);
  }
  StorageOptions o;
  o.path = (dir.path() / "db").string();
  o.sync_mode = SyncMode::kFull;
  auto s = TidesStorage::Open(o);
  ASSERT_TRUE(s.ok()) << s.status().ToString();
  auto txn = (*s)->BeginWrite();
  ASSERT_TRUE(txn.ok());
  ASSERT_TRUE((*txn)
                  ->DeleteRange(ColumnFamily::kReplog, encoding::ReplogKey(0),
                                encoding::ReplogKey(std::numeric_limits<uint64_t>::max()))
                  .ok());
  for (const char* key : {"repl_first", "repl_id", "repl_history"}) {
    ASSERT_TRUE((*txn)->Delete(ColumnFamily::kSys, key).ok());
  }
  ASSERT_TRUE((*txn)->Commit().ok());
}

TEST(ReplicationBaselineIT, APreReplicationMasterNeverContinuesAnEmptyReplica) {
  TempDir dir("tideskv-it-prem6a");
  MakePreReplicationDir(dir);
  if (HasFatalFailure()) return;
  TestServer master(TIDESKV_SERVER_BIN, {.dir = dir.str()});
  EXPECT_NE(master.Stderr().find(kBaselineWarning), std::string::npos) << master.Stderr();
  auto c = master.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"GET", "old"}).str, "v");
  const RespValue r = c->Command({"PSYNC", "?", "1"});
  EXPECT_EQ(r.type, Type::kSimple);
  EXPECT_EQ(r.str.rfind("FULLRESYNC ", 0), 0u) << r.str;
  // Later writes and a restart do not make the log describe the old keys.
  c = master.Connect();
  ASSERT_EQ(c->Command({"SET", "new", "v"}).str, "OK");
  c.reset();
  ASSERT_EQ(master.Terminate(), 0);
  master.Restart();
  c = master.Connect();
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->Command({"PSYNC", "?", "1"}).str.rfind("FULLRESYNC ", 0), 0u);
}

TEST(ReplicationBaselineIT, APreReplicationReplicaIsNeverStreamedOntoItsOldKeys) {
  TestServer master(TIDESKV_SERVER_BIN);
  auto m = master.Connect();
  ASSERT_NE(m, nullptr);
  ASSERT_EQ(m->Command({"SET", "theirs", "v"}).str, "OK");
  TempDir dir("tideskv-it-prem6a");
  MakePreReplicationDir(dir);
  if (HasFatalFailure()) return;
  TestServer replica(
      TIDESKV_SERVER_BIN,
      {.extra_args = {"--replicaof", "127.0.0.1 " + std::to_string(master.port())}, .dir = dir.str()});
  EXPECT_NE(replica.Stderr().find(kBaselineWarning), std::string::npos) << replica.Stderr();
  EXPECT_TRUE(WaitFor([&] { return replica.Stderr().find(kFullResyncLog) != std::string::npos; }, 10s))
      << replica.Stderr();
  EXPECT_GE(InfoField(*m, "stats", "sync_full"), 1);
  EXPECT_EQ(InfoField(*m, "stats", "sync_partial_ok"), 0);
  auto r = replica.Connect();
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(r->Command({"EXISTS", "theirs"}).integer, 0);
  EXPECT_EQ(r->Command({"GET", "old"}).str, "v");
}

}  // namespace
}  // namespace tideskv::test
