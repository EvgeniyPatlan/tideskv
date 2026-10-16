#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tideskv {

class BlockedClient;  // server/blocking.h
class WatchClient;    // server/watch.h

// How another thread reaches a connection: implemented by net::Connection (M4 plan F5).
class ClientLink {
 public:
  virtual ~ClientLink() = default;
  // Thread-safe and non-blocking. Called once when this client's blocked command ends on another thread
  // (served by a push, or CLIENT UNBLOCK): the connection appends `reply` (encoded bytes) and resumes
  // executing its pipelined input.
  virtual void Unblock(std::string reply) = 0;
  // Like Unblock, but the connection is closed once `reply` is written and runs none of its pipelined
  // input (Redis CLIENT_CLOSE_AFTER_REPLY; M6a D14 demotion).
  virtual void UnblockAndClose(std::string reply) = 0;
  // Thread-safe and non-blocking (M7a D21): CLIENT PAUSE ended, so the connection re-dispatches the command
  // it parked (Session::postponed) from the start, then the rest of its pipeline.
  virtual void Resume() = 0;
};

// MULTI state (M5a plan G3).
struct MultiState {
  bool active = false;             // between MULTI and EXEC/DISCARD (Redis CLIENT_MULTI)
  bool dirty_exec = false;         // a command was rejected while queueing: EXEC aborts (CLIENT_DIRTY_EXEC)
  bool has_write = false;          // a queued command has the write flag (Redis mstate.cmd_flags)
  bool has_non_stale = false;      // a queued command lacks the stale flag
  bool has_non_loading = false;    // a queued command lacks the loading flag
  bool has_may_replicate = false;  // a queued write, EVAL or EVALSHA (CLIENT PAUSE WRITE, D21)
  std::vector<std::vector<std::string>> queued;
};

// What a would-be replica told us with REPLCONF (M6a), copied into the replica's entry at PSYNC.
struct ReplicaHandshake {
  int64_t listening_port = 0;  // REPLCONF listening-port (no range check, as in Redis)
  std::string ip_address;      // REPLCONF ip-address; empty = the peer address
};

// Per-connection state. Owned by the connection; touched only on its worker thread.
struct Session {
  uint64_t id = 0;
  std::string peer;                        // "ip:port"
  int protocol = 2;                        // RESP version, switched by HELLO
  uint8_t db = 0;                          // SELECT (M2)
  bool authenticated = false;              // set true at connect when no password is configured
  bool close_after_reply = false;          // QUIT, SHUTDOWN, protocol errors
  std::string name;                        // CLIENT SETNAME
  std::string lib_name;                    // CLIENT SETINFO LIB-NAME
  std::string lib_ver;                     // CLIENT SETINFO LIB-VER
  std::weak_ptr<ClientLink> link;          // set by the connection when it starts; empty in unit tests
  std::shared_ptr<BlockedClient> blocked;  // set while a blocking command waits (M4 F5)
  MultiState multi;                        // M5a G3
  std::shared_ptr<WatchClient> watch;      // created by the first WATCH (M5a G2)
  ReplicaHandshake replica_handshake;      // REPLCONF state (M6a)
  // Set by PSYNC +CONTINUE (M6a D10): once the reply is flushed the connection releases its socket and
  // calls this with the fd and the unread input after PSYNC, then dies without touching the socket again.
  std::function<void(int fd, std::string unread)> takeover;
  bool is_replica_link = false;  // PSYNC succeeded: further PSYNC/SYNC are ignored (Redis CLIENT_SLAVE)
  uint64_t woff = 0;             // Redis c->woff: the seq of this client's last write (WAIT, M6b D9)
  bool readonly_reads = false;   // READONLY (cluster, M7a D13): reads of my master's slots run here
  bool asking = false;           // ASKING: the next command may use an importing slot (consulted from M7b)
  // CLIENT PAUSE (M7a D21): the command Dispatch parked instead of running; the connection executes nothing
  // more until ClientLink::Resume re-dispatches it.
  std::optional<std::vector<std::string>> postponed;
  // Dispatch admitted a may-replicate command that ClientPause counts in flight; DispatchAndServe ends it
  // once the command and the waiters it served are done (M7a D21 barrier).
  bool write_in_flight = false;
};

}  // namespace tideskv
