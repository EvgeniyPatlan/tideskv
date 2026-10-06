// tideskv-spike: M0 engine measurements (spec §11: R1 tombstones, R4 backup, R7
// commit cost). Usage:
//   tideskv-spike commit     --dir D --threads 8 --seconds 10 --sync
//   interval|full|none tideskv-spike tombstones --dir D --collections 100000
//   --fields 20 --deleted-pct 50 tideskv-spike backup     --dir D --gb 1
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "encoding/key_codec.h"
#include "encoding/object_codec.h"
#include "storage/tides_storage.h"

using namespace tideskv;
using tideskv::encoding::EncodeMetaKey;
using tideskv::encoding::EncodeSubkey;
using tideskv::encoding::SubkeyPrefix;
using Clock = std::chrono::steady_clock;

namespace {

std::map<std::string, std::string> ParseFlags(int argc, char **argv) {
  std::map<std::string, std::string> flags;
  for (int i = 2; i + 1 < argc; i += 2) flags[argv[i]] = argv[i + 1];
  return flags;
}

std::string Flag(const std::map<std::string, std::string> &f, const std::string &k, const std::string &def) {
  auto it = f.find(k);
  return it == f.end() ? def : it->second;
}

void Die(const Status &st, const char *what) {
  if (st.ok()) return;
  std::fprintf(stderr, "%s failed: %s\n", what, st.ToString().c_str());
  std::exit(1);
}

std::unique_ptr<TidesStorage> OpenFresh(const std::string &dir, SyncMode sync) {
  std::filesystem::remove_all(dir);
  StorageOptions o;
  o.path = dir;
  o.sync_mode = sync;
  auto s = TidesStorage::Open(o);
  Die(s.status(), "open");
  return std::move(s).value();
}

SyncMode ParseSync(const std::string &s) {
  if (s == "full") return SyncMode::kFull;
  if (s == "none") return SyncMode::kNone;
  return SyncMode::kInterval;
}

double Percentile(std::vector<double> &v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[static_cast<size_t>(p * static_cast<double>(v.size() - 1))];
}

void Commit(WriteTxn &txn, const char *what) { Die(txn.Commit(), what); }

// R7: commit throughput/latency of "one write command" = meta put + replog put
// in one txn.
int RunCommit(const std::map<std::string, std::string> &f) {
  const int threads = std::stoi(Flag(f, "--threads", "4"));
  const int seconds = std::stoi(Flag(f, "--seconds", "10"));
  auto storage = OpenFresh(Flag(f, "--dir", "/tmp/tideskv-spike"), ParseSync(Flag(f, "--sync", "interval")));
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> seq{0};
  std::vector<std::vector<double>> lat(static_cast<size_t>(threads));
  std::vector<std::thread> workers;
  const std::string value(100, 'v');
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      for (uint64_t i = 0; !stop.load(std::memory_order_relaxed); ++i) {
        const auto start = Clock::now();
        auto txn = storage->BeginWrite();
        Die(txn.status(), "begin");
        const std::string key = "t" + std::to_string(t) + ":" + std::to_string(i);
        Die((*txn)->Put(ColumnFamily::kMeta, EncodeMetaKey(0, 0, key), value), "put meta");
        Die((*txn)->Put(ColumnFamily::kReplog, std::to_string(seq.fetch_add(1)), key + value), "put replog");
        Commit(**txn, "commit");
        lat[static_cast<size_t>(t)].push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
      }
    });
  }
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  stop = true;
  for (auto &w : workers) w.join();
  std::vector<double> all;
  for (auto &v : lat) all.insert(all.end(), v.begin(), v.end());
  std::printf(
      "commit threads=%d ops=%zu ops/s=%.0f p50=%.1fus p99=%.1fus "
      "p999=%.1fus\n",
      threads, all.size(), static_cast<double>(all.size()) / seconds, Percentile(all, 0.50), Percentile(all, 0.99),
      Percentile(all, 0.999));
  return 0;
}

struct ReadStats {
  double get_p50, get_p99, scan_p50, scan_p99;
};

ReadStats MeasureReads(TidesStorage &s, const std::vector<uint64_t> &live, uint64_t fields) {
  std::mt19937_64 rng(42);
  std::vector<double> gets, scans;
  for (int i = 0; i < 20000; ++i) {
    const uint64_t oid = live[rng() % live.size()];
    auto view = s.BeginRead();
    Die(view.status(), "begin read");
    const auto start = Clock::now();
    auto v = (*view)->Get(ColumnFamily::kSubkey, EncodeSubkey(0, oid, "f" + std::to_string(rng() % fields)));
    gets.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
    Die(v.status(), "get");
  }
  for (int i = 0; i < 2000; ++i) {
    const uint64_t oid = live[rng() % live.size()];
    auto view = s.BeginRead();
    Die(view.status(), "begin read");
    const auto start = Clock::now();
    const std::string prefix = SubkeyPrefix(0, oid);
    auto it = (*view)->NewIterator(ColumnFamily::kSubkey, {prefix, SubkeyPrefix(0, oid + 1)});
    Die(it.status(), "iter");
    uint64_t n = 0;
    for (Die((*it)->SeekToFirst(), "seek"); (*it)->Valid(); Die((*it)->Next(), "next")) ++n;
    scans.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
    if (n != fields) std::fprintf(stderr, "WARNING: scan of oid %lu saw %lu fields\n", oid, n);
  }
  return {Percentile(gets, .5), Percentile(gets, .99), Percentile(scans, .5), Percentile(scans, .99)};
}

void PrintReads(const char *phase, const ReadStats &r) {
  std::printf("%-22s get p50=%.1fus p99=%.1fus | scan p50=%.1fus p99=%.1fus\n", phase, r.get_p50, r.get_p99, r.scan_p50,
              r.scan_p99);
}

// R1: read cost on surviving collections after many delete_prefix tombstones.
int RunTombstones(const std::map<std::string, std::string> &f) {
  const uint64_t collections = std::stoull(Flag(f, "--collections", "100000"));
  const uint64_t fields = std::stoull(Flag(f, "--fields", "20"));
  const int deleted_pct = std::stoi(Flag(f, "--deleted-pct", "50"));
  auto s = OpenFresh(Flag(f, "--dir", "/tmp/tideskv-spike"), SyncMode::kNone);
  const std::string value(32, 'x');
  for (uint64_t oid = 1; oid <= collections; ++oid) {
    auto txn = s->BeginWrite();
    Die(txn.status(), "begin");
    for (uint64_t i = 0; i < fields; ++i) {
      Die((*txn)->Put(ColumnFamily::kSubkey, EncodeSubkey(0, oid, "f" + std::to_string(i)), value), "put");
    }
    Commit(**txn, "commit load");
  }
  std::vector<uint64_t> live;
  for (uint64_t oid = 1; oid <= collections; ++oid) live.push_back(oid);
  PrintReads("baseline", MeasureReads(*s, live, fields));

  std::vector<uint64_t> survivors;
  const auto delete_start = Clock::now();
  for (uint64_t oid : live) {
    if (static_cast<int>(oid % 100) < deleted_pct) {
      auto txn = s->BeginWrite();
      Die(txn.status(), "begin");
      Die((*txn)->DeletePrefix(ColumnFamily::kSubkey, SubkeyPrefix(0, oid)), "delete_prefix");
      Commit(**txn, "commit delete");
    } else {
      survivors.push_back(oid);
    }
  }
  std::printf("delete phase: %.1fs\n", std::chrono::duration<double>(Clock::now() - delete_start).count());
  PrintReads("after delete_prefix", MeasureReads(*s, survivors, fields));
  Die(s->Checkpoint(), "checkpoint");
  PrintReads("after flush", MeasureReads(*s, survivors, fields));
  return 0;
}

// R4: tidesdb_backup throughput (copy, not hardlink) — basis of replication
// full sync.
int RunBackup(const std::map<std::string, std::string> &f) {
  const uint64_t gb = std::stoull(Flag(f, "--gb", "1"));
  const std::string dir = Flag(f, "--dir", "/tmp/tideskv-spike");
  auto s = OpenFresh(dir, SyncMode::kNone);
  const std::string value(4096, 'b');
  const uint64_t rows = gb * (1ULL << 30) / value.size();
  for (uint64_t i = 0; i < rows; i += 1000) {
    auto txn = s->BeginWrite();
    Die(txn.status(), "begin");
    for (uint64_t j = i; j < std::min(rows, i + 1000); ++j) {
      Die((*txn)->Put(ColumnFamily::kMeta, EncodeMetaKey(0, 0, "k" + std::to_string(j)), value), "put");
    }
    Commit(**txn, "commit");
  }
  Die(s->Checkpoint(), "checkpoint");
  const std::string backup_dir = dir + "-backup";
  std::filesystem::remove_all(backup_dir);
  const auto start = Clock::now();
  Die(s->Backup(backup_dir), "backup");
  const double secs = std::chrono::duration<double>(Clock::now() - start).count();
  uint64_t bytes = 0;
  for (const auto &e : std::filesystem::recursive_directory_iterator(backup_dir)) {
    if (e.is_regular_file()) bytes += e.file_size();
  }
  std::printf("backup bytes=%lu seconds=%.2f MB/s=%.1f\n", bytes, secs, static_cast<double>(bytes) / secs / 1e6);
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: tideskv-spike commit|tombstones|backup [--flag value ...]\n");
    return 2;
  }
  const std::string mode = argv[1];
  const auto flags = ParseFlags(argc, argv);
  if (mode == "commit") return RunCommit(flags);
  if (mode == "tombstones") return RunTombstones(flags);
  if (mode == "backup") return RunBackup(flags);
  std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
  return 2;
}
