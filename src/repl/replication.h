#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "repl/repl_state.h"
#include "server/session.h"

namespace tideskv {

class BlockedClient;  // server/blocking.h

enum class ReplRole : uint8_t { kMaster, kReplica };

// A replica's link to its master (M6a D19), as ROLE names it (redis72-replication §2). kSync: loading a
// full synchronisation (M6b D5), between the handshake and the connected stream.
enum class ReplicaLinkState : uint8_t { kConnect, kConnecting, kHandshake, kSync, kConnected };
inline std::string_view LinkStateName(ReplicaLinkState s) {
  switch (s) {
    case ReplicaLinkState::kConnect:
      return "connect";
    case ReplicaLinkState::kConnecting:
      return "connecting";
    case ReplicaLinkState::kHandshake:
      return "handshake";
    case ReplicaLinkState::kSync:
      return "sync";
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
  bool online = false;  // past its full sync (M6b D10): ROLE lists it, WAIT counts it
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

// The outcome of PSYNC (D8). pending_id names the replica until its socket is handed over (both
// decisions: a FULLRESYNC link streams the snapshot, M6b D2).
struct PsyncResult {
  repl::PsyncDecision decision = repl::PsyncDecision::kFullResync;
  std::string replid;       // this node's replid
  uint64_t seq = 0;         // kFullResync: the snapshot's seq S (the master's last seq when it was taken)
  uint64_t pending_id = 0;  // pass to AttachReplica
  std::string error;        // non-empty: PSYNC is refused with this reply; no decision was taken
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
  // Decides PSYNC (D8) and registers a pending replica that AttachReplica completes (on kFullResync with
  // the snapshot it streams first).
  // Refused with PsyncResult::error, and nothing decided, on a replica (F8) or beyond maxclients (F4).
  virtual PsyncResult HandlePsync(const repl::PsyncRequest& req, const ReplicaPeer& peer) = 0;
  // Called by the connection once the +CONTINUE / +FULLRESYNC reply has been flushed and asio has released
  // the socket: takes ownership of fd (closes it if pending_id is unknown or replication is shut down).
  // unread holds the bytes the client sent after PSYNC; they belong to the replication stream.
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
  // MASTERDOWN (M6b D8): a replica whose link is not connected (sync included) while
  // replica-serve-stale-data is no. Lock-free.
  virtual bool StaleRefusing() const = 0;

  // min-replicas-to-write (M6b D12): true on a replica, when either config is 0, or when at least
  // min-replicas-to-write online replicas acknowledged within min-replicas-max-lag seconds.
  virtual bool GoodReplicasOk() const = 0;

  // WAIT (M6b D9): how many online replicas (past their full sync) last acknowledged a seq >= `seq`.
  virtual int64_t CountAcked(uint64_t seq) const = 0;
  // Parks `client` (already registered in the BlockingRegistry) until CountAcked(seq) >= numreplicas; it
  // is then answered `:<count>` from the ACK path. A timeout, CLIENT UNBLOCK or disconnect ends it first.
  virtual void AddWaiter(std::shared_ptr<BlockedClient> client, uint64_t seq, int64_t numreplicas) = 0;
};

}  // namespace tideskv
