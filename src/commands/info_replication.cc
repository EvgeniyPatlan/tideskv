#include <string>

#include "commands/info.h"

namespace tideskv {
namespace {

void Add(std::string& out, const std::string& key, const std::string& value) {
  out += key;
  out += ':';
  out += value;
  out += "\r\n";
}

void AddReplicaFields(std::string& out, const ReplicationStats& r) {
  Add(out, "master_host", r.master_host);
  Add(out, "master_port", std::to_string(r.master_port));
  Add(out, "master_link_status", r.link_up ? "up" : "down");
  Add(out, "master_last_io_seconds_ago", std::to_string(r.last_io_seconds_ago));
  Add(out, "master_sync_in_progress", r.sync_in_progress ? "1" : "0");
  Add(out, "slave_read_repl_offset", std::to_string(r.read_seq));
  Add(out, "slave_repl_offset", std::to_string(r.applied_seq));
  if (r.sync_in_progress) {
    Add(out, "master_sync_total_bytes", std::to_string(r.sync_total_bytes));
    Add(out, "master_sync_read_bytes", std::to_string(r.sync_read_bytes));
    Add(out, "master_sync_left_bytes", std::to_string(r.sync_total_bytes - r.sync_read_bytes));
    Add(out, "master_sync_perc", "0.00");  // total is 0 (unknown size), server.c:6045
    Add(out, "master_sync_last_io_seconds_ago", std::to_string(r.sync_last_io_seconds_ago));
  }
  if (!r.link_up) Add(out, "master_link_down_since_seconds", std::to_string(r.link_down_since_seconds));
  Add(out, "slave_priority", std::to_string(r.replica_priority));
  Add(out, "slave_read_only", r.read_only ? "1" : "0");
  Add(out, "replica_announced", "1");
}

}  // namespace

// repl_backlog_active is 0 while repl-backlog-lazy keeps the replog dormant (no replica yet, as Redis
// creates its backlog for the first one); the backlog offsets are seqs and histlen counts retained batches.
std::string FormatReplicationSection(const ReplicationStats& r) {
  std::string out = "# Replication\r\n";
  Add(out, "role", r.replica ? "slave" : "master");
  if (r.replica) AddReplicaFields(out, r);
  Add(out, "connected_slaves", std::to_string(r.replicas.size()));
  if (r.min_replicas_enabled) Add(out, "min_slaves_good_slaves", std::to_string(r.good_replicas));
  for (size_t i = 0; i < r.replicas.size(); ++i) {
    const ReplicaLine& l = r.replicas[i];
    Add(out, "slave" + std::to_string(i),
        "ip=" + l.ip + ",port=" + std::to_string(l.port) + ",state=" + l.state +
            ",offset=" + std::to_string(l.offset) + ",lag=" + std::to_string(l.lag_seconds));
  }
  Add(out, "master_failover_state", "no-failover");
  Add(out, "master_replid", r.replid);
  Add(out, "master_replid2", r.replid2.empty() ? std::string(r.replid.size(), '0') : r.replid2);
  Add(out, "master_repl_offset", std::to_string(r.master_offset));
  Add(out, "second_repl_offset", std::to_string(r.second_offset));
  Add(out, "repl_backlog_active", r.backlog_active ? "1" : "0");
  Add(out, "repl_backlog_size", std::to_string(r.backlog_size_config));
  Add(out, "repl_backlog_first_byte_offset", std::to_string(r.first_retained));
  Add(out, "repl_backlog_histlen", std::to_string(r.histlen));
  return out;
}

}  // namespace tideskv
