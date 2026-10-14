#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "repl/repl_state.h"
#include "server/session.h"

namespace tideskv {

enum class ReplRole : uint8_t { kMaster, kReplica };

// A replica's link to its master (M6a D19), as ROLE names it (redis72-replication §2).
enum class ReplicaLinkState : uint8_t { kConnect, kConnecting, kHandshake, kConnected };
inline std::string_view LinkStateName(ReplicaLinkState s) {
  switch (s) {
    case ReplicaLinkState::kConnect:
      return "connect";
    case ReplicaLinkState::kConnecting:
      return "connecting";
    case ReplicaLinkState::kHandshake:
      return "handshake";
    case ReplicaLinkState::kConnected:
      return "connected";
  }
  return "none";
}

// One attached replica, as the master sees it.
struct ReplicaInfo {
  std::string ip;
  uint16_t listening_port = 0;
  uint64_t acked = 0;
  int64_t last_ack_ms = 0;
  bool alive = false;
};

// ROLE's data (redis72-replication §2). A master fills offset and replicas; a replica fills host, port,
// state and offset_or_minus1 (its applied seq while the link is connected, else -1).
struct RoleInfo {
  ReplRole role = ReplRole::kMaster;
  std::string host;
  uint16_t port = 0;
  ReplicaLinkState state = ReplicaLinkState::kConnect;
  uint64_t offset = 0;  // the last seq in the log (applied or committed)
  int64_t offset_or_minus1 = -1;
  std::vector<ReplicaInfo> replicas;
};

// Where a replica says it can be reached (REPLCONF ip-address / listening-port, else the peer address).
struct ReplicaPeer {
  std::string ip;
  uint16_t listening_port = 0;
};

// The outcome of PSYNC (D8). pending_id names the replica until its socket is handed over (kContinue).
struct PsyncResult {
  repl::PsyncDecision decision = repl::PsyncDecision::kFullResync;
  std::string replid;       // this node's replid
  uint64_t seq = 0;         // kFullResync: the master's last seq
  uint64_t pending_id = 0;  // kContinue: pass to AttachReplica
};

// One REPLCONF call's options, after validation (redis72-replication §6).
struct ReplconfOptions {
  std::optional<int64_t> listening_port;
  std::optional<std::string> ip_address;
  std::vector<std::string> capa;  // every capa value, recognised or not
  bool getack = false;
};

// What command handlers ask of replication (M6a). Implemented by repl::ReplicationManager and
// test::FakeReplication. Every method is thread-safe and never blocks on a socket or the engine.
class Replication {
 public:
  virtual ~Replication() = default;
  virtual ReplRole role() const = 0;  // one atomic load
  bool IsReplica() const { return role() == ReplRole::kReplica; }
  // Decides PSYNC (D8) and, on kContinue, registers a pending replica that AttachReplica completes.
  virtual PsyncResult HandlePsync(const repl::PsyncRequest& req, const ReplicaPeer& peer) = 0;
  // Called by the connection once the +CONTINUE reply has been flushed and asio has released the socket:
  // takes ownership of fd (closes it if pending_id is unknown or replication is shut down). unread holds
  // the bytes the client sent after PSYNC; they belong to the replication stream.
  virtual void AttachReplica(uint64_t pending_id, int fd, std::string unread) = 0;
  // A valid REPLCONF from session s (listening-port / ip-address / capa / getack), already recorded in
  // s.replica_handshake by the command.
  virtual void OnReplconf(Session& s, const ReplconfOptions& o) = 0;

  // Role changes (M6a D7, D13, D14). Both run on a command thread and never block on the network; they
  // may wait for replication threads and the expire reaper to stop, so never call them while holding the
  // Engine gate (inside EXEC they run after the batch).
  // REPLICAOF host port. False when already replicating that exact master (case-insensitive host).
  virtual bool BecomeReplica(const std::string& host, uint16_t port) = 0;
  // REPLICAOF NO ONE; a no-op on a master. A storage error (persisting the new identity, reloading the
  // oid high-water mark) is returned and leaves the node a fenced replica of the same master.
  virtual Status BecomeMaster() = 0;
  virtual RoleInfo Role() const = 0;
};

}  // namespace tideskv
