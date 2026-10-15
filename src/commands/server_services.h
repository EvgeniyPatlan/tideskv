#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "server/config.h"

namespace tideskv {

class BlockingRegistry;
class CommandRegistry;
class CursorStore;
class Engine;
class OidAllocator;
class Replication;
class ScriptEngine;
class WatchRegistry;

// One attached replica as INFO lists it (state is always "online": only streaming replicas are tracked).
struct ReplicaLine {
  std::string ip;
  int port = 0;
  uint64_t offset = 0;           // last REPLCONF ACK (a seq)
  int64_t lag_seconds = 0;       // now - last ACK
  std::string state = "online";  // "send_bulk" until the sender is online (offset and lag 0 then)
};

// What INFO's Replication, Keyspace `sequence` and sync counters show (M6a D16). Offsets are TidesKV seqs.
struct ReplicationStats {
  bool replica = false;
  std::string master_host;
  int master_port = 0;
  bool link_up = false;
  int64_t last_io_seconds_ago = -1;
  int64_t link_down_since_seconds = -1;  // -1 = never connected since REPLICAOF
  bool sync_in_progress = false;         // a full sync is being received (M6b D11)
  int64_t sync_total_bytes = 0;          // always 0: the snapshot size is unknown (server.c:6045)
  int64_t sync_read_bytes = 0;
  int64_t sync_last_io_seconds_ago = 0;
  uint64_t applied_seq = 0;
  uint64_t read_seq = 0;
  int replica_priority = 100;
  bool read_only = true;
  std::vector<ReplicaLine> replicas;
  std::string replid;
  std::string replid2;         // empty = unset (shown as 40 zeros)
  int64_t second_offset = -1;  // newest history end_seq + 1, or -1
  uint64_t master_offset = 0;  // the log's last seq
  uint64_t backlog_size_config = 0;
  uint64_t first_retained = 1;
  uint64_t histlen = 0;
  uint64_t sync_full = 0;
  uint64_t sync_partial_ok = 0;
  uint64_t sync_partial_err = 0;
};

struct ServerStats {
  ReplicationStats repl;
  std::string version;
  int64_t pid = 0;
  uint16_t tcp_port = 0;
  int64_t uptime_seconds = 0;
  int workers = 0;
  int maxclients = 0;
  uint64_t connected_clients = 0;
  uint64_t total_connections = 0;
  uint64_t total_commands = 0;
  std::string data_dir;
  std::string sync_mode;
  std::string storage_compression;  // "no", "lz4" or "zstd"
  // Entries in sstables, all CFs but the replog (dead ones until compaction drops them).
  uint64_t storage_keys = 0;
  uint64_t storage_tombstones = 0;
  uint64_t storage_flushes = 0;
  uint64_t storage_compactions = 0;
  uint64_t background_compactions = 0;          // CompactionScheduler jobs completed
  uint64_t background_compaction_failures = 0;  // jobs given up after retries
  uint64_t expired_keys = 0;                    // removed by the active-expiry reaper
  uint64_t blocked_clients = 0;                 // clients waiting in a blocking command
  uint64_t lua_memory = 0;                      // bytes held by the Lua VM
  uint64_t cached_scripts = 0;
  uint64_t blocking_keys = 0;         // distinct keys with at least one blocked client
  bool loading = false;               // INFO loading (M6b D11)
  int64_t loading_start_time = 0;     // unix seconds
  uint64_t loading_loaded_bytes = 0;  // LOAD payload bytes applied
  int64_t loading_eta_seconds = 0;    // Redis's formula (server.c:5850), computed in App::Stats
};

// What command handlers may ask of the server. Implemented by App (Task 19) and
// by test::FakeServerServices.
class ServerServices {
 public:
  virtual ~ServerServices() = default;
  virtual Config& config() = 0;
  virtual const CommandRegistry& registry() const = 0;
  virtual ServerStats Stats() const = 0;
  virtual void ResetStats() = 0;
  virtual void RequestShutdown() = 0;        // must not block; the main thread performs the shutdown
  virtual Engine& engine() = 0;              // data access for commands (Task 5)
  virtual OidAllocator& oids() = 0;          // collection object ids (M3 E1)
  virtual CursorStore& cursors() = 0;        // SCAN cursor table (Task 14)
  virtual BlockingRegistry& blocking() = 0;  // blocked clients (M4 F4)
  virtual WatchRegistry& watches() = 0;      // WATCH state (M5a G2)
  virtual ScriptEngine& scripts() = 0;       // EVAL / SCRIPT (M5b)
  virtual Replication& replication() = 0;    // role queries (M6a)
};

}  // namespace tideskv
