#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "repl/repl_state.h"
#include "server/session.h"

namespace tideskv {

enum class ReplRole : uint8_t { kMaster, kReplica };

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
};

}  // namespace tideskv
