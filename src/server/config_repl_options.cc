#include <cstdint>
#include <limits>
#include <string>

#include "common/string_util.h"
#include "server/config_options.h"

namespace tideskv {
namespace {

Status OutOfRange(int64_t lo, int64_t hi) {
  return Status::InvalidArgument("argument must be between " + std::to_string(lo) + " and " +
                                 std::to_string(hi) + " inclusive");
}

template <typename T>
Status SetBounded(std::string_view v, int64_t lo, int64_t hi, T* out) {
  int64_t n = 0;
  if (!ParseInt64(v, &n) || n < lo || n > hi) return OutOfRange(lo, hi);
  *out = static_cast<T>(n);
  return Status::Ok();
}

Status ParseYesNo(std::string_view v, bool* out) {
  const std::string lower = ToLower(v);
  if (lower != "yes" && lower != "no") return Status::InvalidArgument("argument must be 'yes' or 'no'");
  *out = lower == "yes";
  return Status::Ok();
}

Status SetText(std::string_view v, std::string* out) {
  for (const char ch : v) {
    const auto u = static_cast<uint8_t>(ch);
    if (u < 0x20 || u == 0x7f) {
      return Status::InvalidArgument("argument must not contain control characters");
    }
  }
  *out = std::string(v);
  return Status::Ok();
}

// "<host> <port>" or "no one" (clears it), as in Redis's setConfigReplicaOfOption.
Status SetReplicaOf(ConfigValues& c, std::string_view v) {
  std::vector<std::string> words;
  for (size_t i = 0; i < v.size();) {
    const size_t b = v.find_first_not_of(" \t", i);
    if (b == std::string_view::npos) break;
    const size_t e = v.find_first_of(" \t", b);
    words.emplace_back(v.substr(b, e == std::string_view::npos ? std::string_view::npos : e - b));
    i = e == std::string_view::npos ? v.size() : e;
  }
  if (words.size() != 2) return Status::InvalidArgument("wrong number of arguments");
  if (EqualsIgnoreCase(words[0], "no") && EqualsIgnoreCase(words[1], "one")) {
    c.replicaof.clear();
    return Status::Ok();
  }
  int64_t port = 0;
  if (!ParseInt64(words[1], &port) || port < 0 || port > 65535) {
    return Status::InvalidArgument("Invalid master port");
  }
  std::string host;
  if (Status st = SetText(words[0], &host); !st.ok()) return st;
  c.replicaof = host + " " + std::to_string(port);
  return Status::Ok();
}

Status SetReadOnly(ConfigValues& c, std::string_view v) {
  bool on = false;
  if (Status st = ParseYesNo(v, &on); !st.ok()) return st;
  if (!on) return Status::InvalidArgument("writable replicas are not supported");
  c.replica_read_only = true;
  return Status::Ok();
}

Status SetBacklogSize(ConfigValues& c, std::string_view v) {
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  uint64_t bytes = 0;
  if (!ParseMemory(v, &bytes) || bytes < 1 || bytes > static_cast<uint64_t>(kMax)) return OutOfRange(1, kMax);
  c.repl_backlog_size = bytes;
  return Status::Ok();
}

constexpr int64_t kIntMax = std::numeric_limits<int32_t>::max();
constexpr int64_t kIntMin = std::numeric_limits<int32_t>::min();

const std::vector<Option> BuildReplOptions() {
  std::vector<Option> opts = {
      {"masterauth", true, [](ConfigValues& c, std::string_view v) { return SetText(v, &c.masterauth); },
       [](const ConfigValues& c) { return c.masterauth; }},
      {"masteruser", true, [](ConfigValues& c, std::string_view v) { return SetText(v, &c.masteruser); },
       [](const ConfigValues& c) { return c.masteruser; }},
      {"repl-backlog-size", true, SetBacklogSize,
       [](const ConfigValues& c) { return std::to_string(c.repl_backlog_size); }},
      {"repl-backlog-ttl", true,
       [](ConfigValues& c, std::string_view v) {
         return SetBounded(v, 0, std::numeric_limits<int64_t>::max(), &c.repl_backlog_ttl);
       },
       [](const ConfigValues& c) { return std::to_string(c.repl_backlog_ttl); }},
      // Startup only: the replication log's mode is chosen when it is recovered.
      {"repl-backlog-lazy", false,
       [](ConfigValues& c, std::string_view v) { return ParseYesNo(v, &c.repl_backlog_lazy); },
       [](const ConfigValues& c) { return std::string(c.repl_backlog_lazy ? "yes" : "no"); }},
      {"repl-timeout", true,
       [](ConfigValues& c, std::string_view v) { return SetBounded(v, 1, kIntMax, &c.repl_timeout); },
       [](const ConfigValues& c) { return std::to_string(c.repl_timeout); }},
      {"replica-announce-ip", true,
       [](ConfigValues& c, std::string_view v) { return SetText(v, &c.replica_announce_ip); },
       [](const ConfigValues& c) { return c.replica_announce_ip; }},
      {"replica-announce-port", true,
       [](ConfigValues& c, std::string_view v) { return SetBounded(v, 0, 65535, &c.replica_announce_port); },
       [](const ConfigValues& c) { return std::to_string(c.replica_announce_port); }},
      {"replica-priority", true,
       [](ConfigValues& c, std::string_view v) { return SetBounded(v, 0, kIntMax, &c.replica_priority); },
       [](const ConfigValues& c) { return std::to_string(c.replica_priority); }},
      {"replica-read-only", true, SetReadOnly,
       [](const ConfigValues& c) { return std::string(c.replica_read_only ? "yes" : "no"); }},
      {"replicaof", false, SetReplicaOf, [](const ConfigValues& c) { return c.replicaof; }, false, true},
      {"replica-serve-stale-data", true,
       [](ConfigValues& c, std::string_view v) { return ParseYesNo(v, &c.replica_serve_stale_data); },
       [](const ConfigValues& c) { return std::string(c.replica_serve_stale_data ? "yes" : "no"); }},
      {"min-replicas-to-write", true,
       [](ConfigValues& c, std::string_view v) {
         return SetBounded(v, 0, kIntMax, &c.min_replicas_to_write);
       },
       [](const ConfigValues& c) { return std::to_string(c.min_replicas_to_write); }},
      {"min-replicas-max-lag", true,
       [](ConfigValues& c, std::string_view v) { return SetBounded(v, 0, kIntMax, &c.min_replicas_max_lag); },
       [](const ConfigValues& c) { return std::to_string(c.min_replicas_max_lag); }},
      {"key-load-delay", true,
       [](ConfigValues& c, std::string_view v) { return SetBounded(v, kIntMin, kIntMax, &c.key_load_delay); },
       [](const ConfigValues& c) { return std::to_string(c.key_load_delay); }, false, false, true},
      {"rdb-key-save-delay", true,
       [](ConfigValues& c, std::string_view v) {
         return SetBounded(v, kIntMin, kIntMax, &c.rdb_key_save_delay);
       },
       [](const ConfigValues& c) { return std::to_string(c.rdb_key_save_delay); }, false, false, true},
      // M7a D19: startup-only. Lives here for ParseYesNo; it is the one cluster option.
      {"cluster-enabled", false,
       [](ConfigValues& c, std::string_view v) { return ParseYesNo(v, &c.cluster_enabled); },
       [](const ConfigValues& c) { return std::string(c.cluster_enabled ? "yes" : "no"); }},
      // M7b D17 (Kvrocks config.cc:241-243), D25 (hidden).
      {"migrate-sequence-gap", true,
       [](ConfigValues& c, std::string_view v) { return SetBounded(v, 1, kIntMax, &c.migrate_sequence_gap); },
       [](const ConfigValues& c) { return std::to_string(c.migrate_sequence_gap); }},
      {"migrate-batch-size-kb", true,
       [](ConfigValues& c, std::string_view v) {
         return SetBounded(v, 1, kIntMax, &c.migrate_batch_size_kb);
       },
       [](const ConfigValues& c) { return std::to_string(c.migrate_batch_size_kb); }},
      {"migrate-batch-rate-limit-mb", true,
       [](ConfigValues& c, std::string_view v) {
         return SetBounded(v, 1, kIntMax, &c.migrate_batch_rate_limit_mb);
       },
       [](const ConfigValues& c) { return std::to_string(c.migrate_batch_rate_limit_mb); }},
      {"migrate-delay-ms", true,
       [](ConfigValues& c, std::string_view v) { return SetBounded(v, 0, kIntMax, &c.migrate_delay_ms); },
       [](const ConfigValues& c) { return std::to_string(c.migrate_delay_ms); }, false, false, true},
  };
  static constexpr std::pair<std::string_view, std::string_view> kAliases[] = {
      {"slaveof", "replicaof"},
      {"slave-read-only", "replica-read-only"},
      {"slave-announce-ip", "replica-announce-ip"},
      {"slave-announce-port", "replica-announce-port"},
      {"slave-priority", "replica-priority"},
      {"slave-serve-stale-data", "replica-serve-stale-data"},
      {"min-slaves-to-write", "min-replicas-to-write"},
      {"min-slaves-max-lag", "min-replicas-max-lag"},
  };
  const size_t base = opts.size();
  for (const auto& [alias, target] : kAliases) {
    for (size_t i = 0; i < base; ++i) {
      if (opts[i].name != target) continue;
      Option copy = opts[i];
      copy.name = alias;
      copy.alias = true;
      opts.push_back(copy);
    }
  }
  return opts;
}

}  // namespace

const std::vector<Option>& ReplOptions() {
  static const std::vector<Option> kOptions = BuildReplOptions();
  return kOptions;
}

}  // namespace tideskv
