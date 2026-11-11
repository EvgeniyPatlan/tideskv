// Commit gate: kill -9 of a TidesDB writer pool mid-commit, through the public db.h API only (no server).
// A rotation must never split a commit's WAL record from its apply (the 10.1.1 race the commit-gate patch
// closes). Child: W writers commit txn i = kKeys puts into CF "data" plus one put into CF "replog", then
// pwrite i+1 to a per-writer ack file (survives the kill). Parent: SIGKILL at a random time, reopen, check
// per writer that nothing acknowledged is missing and no txn is torn, split across CFs, or has a hole.
#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "support/temp_dir.h"

extern "C" {
#include <db.h>
}

namespace tideskv::test {
namespace {

constexpr int kKeys = 64;  // data puts per txn
constexpr int kValueLen = 100;
constexpr int kDefaultIters = 3;
constexpr int kKillBaseMs = 300;
constexpr int kKillJitterMs = 1500;
constexpr int kFirstAckTimeoutS = 30;  // bound on the wait for the first acknowledged commit
constexpr int kFlushEveryUs = 3000;
constexpr size_t kAckSlack = 200;  // txns past the ack to inspect: committed but not yet acknowledged
constexpr int kChildSetupFailed = 3;

// TIDESKV_GATE_CRASH_ITERS sets the kill rounds per test; anything outside 1-1000 keeps the default.
int Iterations() {
  const char* env = std::getenv("TIDESKV_GATE_CRASH_ITERS");
  if (env == nullptr) return kDefaultIters;
  char* end = nullptr;
  const long parsed = std::strtol(env, &end, 10);
  if (end == env || *end != '\0' || parsed < 1 || parsed > 1000) return kDefaultIters;
  return static_cast<int>(parsed);
}

struct Scenario {
  int sync_mode;
  size_t memtable_bytes;
  int writers;
  bool forced_rotations;
};

void PutBe64(uint64_t v, uint8_t* out) {
  for (int i = 7; i >= 0; --i) {
    out[i] = static_cast<uint8_t>(v & 0xff);
    v >>= 8;
  }
}

struct DataKey {
  uint8_t bytes[10];
  DataKey(int writer, uint64_t txn, int j) {
    bytes[0] = static_cast<uint8_t>(writer);
    PutBe64(txn, bytes + 1);
    bytes[9] = static_cast<uint8_t>(j);
  }
};

struct ReplogKey {
  uint8_t bytes[9];
  ReplogKey(int writer, uint64_t txn) {
    bytes[0] = static_cast<uint8_t>(writer);
    PutBe64(txn, bytes + 1);
  }
};

tidesdb_t* OpenDb(const std::string& path, const Scenario& sc) {
  tidesdb_config_t cfg = tidesdb_default_config();
  std::string p = path;
  cfg.db_path = p.data();
  cfg.log_level = TDB_LOG_ERROR;
  cfg.memtable_sync_mode = sc.sync_mode;
  cfg.memtable_write_buffer_size = sc.memtable_bytes;
  tidesdb_t* db = nullptr;
  return tidesdb_open(&cfg, &db) == TDB_SUCCESS ? db : nullptr;
}

tidesdb_column_family_t* EnsureCf(tidesdb_t* db, const char* name) {
  if (tidesdb_column_family_t* cf = tidesdb_get_column_family(db, name)) return cf;
  tidesdb_column_family_config_t cc = tidesdb_default_column_family_config();
  const int rc = tidesdb_create_column_family(db, name, &cc);
  if (rc != TDB_SUCCESS && rc != TDB_ERR_EXISTS) return nullptr;
  return tidesdb_get_column_family(db, name);
}

// One writer: commits forever; the process is killed from outside. Any API failure ends the child with a
// non-signal exit, which the parent reports.
void WriterLoop(tidesdb_t* db, tidesdb_column_family_t* data, tidesdb_column_family_t* replog, int writer,
                int ack_fd) {
  const std::vector<uint8_t> value(kValueLen, 'd');
  for (uint64_t i = 0;; ++i) {
    tidesdb_txn_t* txn = nullptr;
    if (tidesdb_txn_begin_with_isolation(db, TDB_ISOLATION_READ_COMMITTED, &txn) != TDB_SUCCESS) _exit(4);
    for (int j = 0; j < kKeys; ++j) {
      const DataKey k(writer, i, j);
      if (tidesdb_txn_put(txn, data, k.bytes, sizeof k.bytes, value.data(), kValueLen, 0) != TDB_SUCCESS)
        _exit(5);
    }
    const ReplogKey rk(writer, i);
    if (tidesdb_txn_put(txn, replog, rk.bytes, sizeof rk.bytes, value.data(), 10, 0) != TDB_SUCCESS) _exit(6);
    if (tidesdb_txn_commit(txn) != TDB_SUCCESS) _exit(7);
    tidesdb_txn_free(txn);
    const uint64_t acked = i + 1;
    if (pwrite(ack_fd, &acked, sizeof acked, 0) != static_cast<ssize_t>(sizeof acked)) _exit(8);
  }
}

[[noreturn]] void RunChild(const std::string& db_path, const std::vector<std::string>& ack_paths,
                           const Scenario& sc) {
  std::vector<int> ack_fds;
  for (const std::string& p : ack_paths) {
    const int fd = open(p.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd < 0) _exit(kChildSetupFailed);
    ack_fds.push_back(fd);
  }
  tidesdb_t* db = OpenDb(db_path, sc);
  if (db == nullptr) _exit(kChildSetupFailed);
  tidesdb_column_family_t* data = EnsureCf(db, "data");
  tidesdb_column_family_t* replog = EnsureCf(db, "replog");
  if (data == nullptr || replog == nullptr) _exit(kChildSetupFailed);
  std::vector<std::thread> threads;
  for (int w = 0; w < sc.writers; ++w) threads.emplace_back(WriterLoop, db, data, replog, w, ack_fds[w]);
  if (sc.forced_rotations) {
    threads.emplace_back([db] {
      for (;;) {
        tidesdb_flush_memtable(db);
        usleep(kFlushEveryUs);
      }
    });
  }
  for (;;) pause();
}

uint64_t ReadAck(const std::string& path) {
  uint64_t n = 0;
  const int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) return 0;
  if (pread(fd, &n, sizeof n, 0) != static_cast<ssize_t>(sizeof n)) n = 0;
  close(fd);
  return n;
}

struct WriterCounts {
  uint64_t torn = 0, split = 0, missing_acked = 0, holes = 0;
  bool Clean() const { return torn == 0 && split == 0 && missing_acked == 0 && holes == 0; }
};

WriterCounts CheckWriter(tidesdb_txn_t* txn, tidesdb_column_family_t* data, tidesdb_column_family_t* replog,
                         int writer, uint64_t acked) {
  WriterCounts c;
  bool seen_absent = false;
  for (uint64_t i = 0; i < acked + kAckSlack; ++i) {
    int present = 0;
    for (int j = 0; j < kKeys; ++j) {
      const DataKey k(writer, i, j);
      if (tidesdb_txn_contains(txn, data, k.bytes, sizeof k.bytes) == TDB_SUCCESS) ++present;
    }
    const ReplogKey rk(writer, i);
    const bool has_replog = tidesdb_txn_contains(txn, replog, rk.bytes, sizeof rk.bytes) == TDB_SUCCESS;
    if (present != 0 && present != kKeys) {
      ++c.torn;
    } else if ((present == kKeys) != has_replog) {
      ++c.split;
    }
    if (has_replog) {
      if (seen_absent) ++c.holes;
    } else {
      seen_absent = true;
      if (i < acked) ++c.missing_acked;
    }
  }
  return c;
}

// Returns the number of writers that failed the check (0 = round clean); -1 if the round itself broke.
int CheckAfterCrash(const std::string& db_path, const std::vector<std::string>& ack_paths, const Scenario& sc,
                    int iter) {
  tidesdb_t* db = OpenDb(db_path, sc);
  if (db == nullptr) {
    ADD_FAILURE() << "iter " << iter << ": reopen after kill -9 failed";
    return -1;
  }
  tidesdb_column_family_t* data = tidesdb_get_column_family(db, "data");
  tidesdb_column_family_t* replog = tidesdb_get_column_family(db, "replog");
  tidesdb_txn_t* txn = nullptr;
  if (data == nullptr || replog == nullptr ||
      tidesdb_txn_begin_with_isolation(db, TDB_ISOLATION_READ_COMMITTED, &txn) != TDB_SUCCESS) {
    ADD_FAILURE() << "iter " << iter << ": column families or read txn missing after kill -9";
    tidesdb_close(db);
    return -1;
  }
  int bad_writers = 0;
  for (int w = 0; w < sc.writers; ++w) {
    const uint64_t acked = ReadAck(ack_paths[w]);
    const WriterCounts c = CheckWriter(txn, data, replog, w, acked);
    if (c.Clean()) continue;
    ++bad_writers;
    ADD_FAILURE() << "iter " << iter << " writer " << w << ": torn=" << c.torn << " split=" << c.split
                  << " missing_acked=" << c.missing_acked << " holes=" << c.holes << " (acked=" << acked
                  << ")";
  }
  tidesdb_txn_free(txn);
  tidesdb_close(db);
  return bad_writers;
}

uint64_t SumAcks(const std::vector<std::string>& ack_paths) {
  uint64_t sum = 0;
  for (const std::string& path : ack_paths) sum += ReadAck(path);
  return sum;
}

void RunScenario(const Scenario& sc) {
  const int iters = Iterations();
  const unsigned seed = static_cast<unsigned>(getpid());
  srand(seed);
  for (int it = 0; it < iters; ++it) {
    TempDir dir("tideskv-gate-crash");
    const std::string db_path = dir.str() + "/db";
    std::vector<std::string> ack_paths;
    for (int w = 0; w < sc.writers; ++w) ack_paths.push_back(dir.str() + "/ack" + std::to_string(w));

    const int kill_after_ms = kKillBaseMs + rand() % kKillJitterMs;
    // every failure below names what reproduces the round
    SCOPED_TRACE("seed " + std::to_string(seed) + ", iter " + std::to_string(it) + ", kill after " +
                 std::to_string(kill_after_ms) + " ms");
    const pid_t pid = fork();
    ASSERT_GE(pid, 0) << "fork failed";
    if (pid == 0) RunChild(db_path, ack_paths, sc);
    // the kill delay counts from the first acknowledged commit, not from the fork: on a loaded host
    // the child's open alone can outlast the delay. A child that never acks within the bound falls
    // through and fails the progress assert below
    const auto ack_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kFirstAckTimeoutS);
    while (SumAcks(ack_paths) == 0 && std::chrono::steady_clock::now() < ack_deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kill_after_ms));
    // read before the kill: a child that never got past open would pass every check below with
    // nothing acknowledged, so the round has to show it was killed mid-commit, not before one
    const uint64_t acked_before_kill = SumAcks(ack_paths);
    kill(pid, SIGKILL);
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)
        << "iter " << it << ": child ended on its own, exit status "
        << (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    ASSERT_GT(acked_before_kill, 0u) << "iter " << it << ": child acknowledged no commit before the kill";

    ASSERT_NE(CheckAfterCrash(db_path, ack_paths, sc, it), -1);
  }
}

constexpr size_t k256KiB = 256 * 1024;
constexpr size_t k1MiB = 1024 * 1024;

TEST(CommitGateCrash, FullFourWriters) { RunScenario({TDB_SYNC_FULL, k256KiB, 4, false}); }
TEST(CommitGateCrash, IntervalFourWriters) { RunScenario({TDB_SYNC_INTERVAL, k1MiB, 4, false}); }
TEST(CommitGateCrash, FullForcedRotations) { RunScenario({TDB_SYNC_FULL, k256KiB, 4, true}); }
TEST(CommitGateCrash, FullEightWriters) { RunScenario({TDB_SYNC_FULL, k256KiB, 8, false}); }
TEST(CommitGateCrash, FullSixteenWriters) { RunScenario({TDB_SYNC_FULL, k256KiB, 16, false}); }

}  // namespace
}  // namespace tideskv::test
