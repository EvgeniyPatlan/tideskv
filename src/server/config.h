#pragma once
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/status.h"

namespace tideskv {

// Defaults per spec §9.
struct ConfigValues {
  std::string bind = "127.0.0.1";
  uint16_t port = 6666;
  int workers = 0;  // 0 = number of CPUs
  std::string dir = "./data";
  std::string sync_mode = "interval";  // none | interval | full
  uint64_t sync_interval_us = 1'000'000;
  uint64_t block_cache_size = 4ULL << 30;
  uint64_t memtable_size = 64ULL << 20;
  std::string requirepass;  // empty = no AUTH required
  int maxclients = 10000;
  uint64_t client_query_buffer_limit = 1ULL << 30;
  std::string compression = "lz4";  // no | lz4 | zstd (startup only)
  std::string loglevel = "notice";  // debug | verbose | notice | warning
  std::string logfile;              // empty = stderr
};

class Config {
 public:
  using Pairs = std::vector<std::pair<std::string, std::string>>;

  Status LoadFile(const std::string& path);                // Redis-style "name value" lines
  Status ApplyArgs(const std::vector<std::string>& args);  // {"--port", "7000", ...}
  Status Set(std::string_view name, std::string_view value, bool at_runtime);
  // All-or-nothing. On failure *failed_name (if non-null) names the offending option.
  Status SetMany(const Pairs& pairs, bool at_runtime, std::string* failed_name);
  Pairs Get(std::string_view pattern) const;  // case-insensitive glob, sorted by name
  Status Rewrite() const;                     // rewrites the loaded file with current values
  ConfigValues Snapshot() const;
  std::string file_path() const;

 private:
  mutable std::shared_mutex mu_;
  mutable std::mutex rewrite_mu_;
  ConfigValues values_;
  std::string file_path_;
};

}  // namespace tideskv
