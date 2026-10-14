#pragma once
#include <cstdint>
#include <string>

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

struct ServerStats {
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
  uint64_t blocking_keys = 0;  // distinct keys with at least one blocked client
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
  virtual Replication& replication() = 0;   // role queries (M6a)
};

}  // namespace tideskv
