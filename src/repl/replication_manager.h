#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "exec/repl_log.h"
#include "repl/repl_sender.h"
#include "repl/repl_state.h"
#include "repl/replication.h"
#include "storage/storage.h"

// Owns the node's replication role and its replica links (M6a D7-D10). This is the master half: PSYNC,
// the socket hand-off and one ReplSender thread per replica. Task 12 adds the replica half.
//
// Threads: a maintenance thread ticks every second to reap dead senders and expire pending hand-offs
// (the Task 14 trimmer is a separate concern, so the tick does not piggyback on it). mu_ guards the
// identity, the pending table and the sender list; it is never held while calling the Engine, while
// blocked on a socket, or while joining a sender thread.
namespace tideskv::repl {

class ReplicationManager final : public Replication {
 public:
  struct Options {
    std::function<std::chrono::milliseconds()> repl_timeout;  // null = 60 s
  };
  struct Counters {
    uint64_t sync_full = 0;         // FULLRESYNC replies
    uint64_t sync_partial_ok = 0;   // CONTINUE replies
    uint64_t sync_partial_err = 0;  // FULLRESYNC to a request that named a replid
    uint64_t connected_replicas = 0;
  };
  struct ReplicaInfo {
    std::string ip;
    uint16_t listening_port = 0;
    uint64_t acked = 0;
    int64_t last_ack_ms = 0;
    bool alive = false;
  };

  ReplicationManager(Storage& storage, ReplLog& log, ReplIdentity identity, Options options = {});
  ~ReplicationManager() override;  // Shutdown()
  ReplicationManager(const ReplicationManager&) = delete;
  ReplicationManager& operator=(const ReplicationManager&) = delete;

  void Start();     // the maintenance thread
  void Shutdown();  // idempotent: stops the tick and every sender; later hand-offs are refused

  ReplRole role() const override { return ReplRole::kMaster; }
  PsyncResult HandlePsync(const PsyncRequest& req, const ReplicaPeer& peer) override;
  void AttachReplica(uint64_t pending_id, int fd, std::string unread) override;
  void OnReplconf(Session& s, const ReplconfOptions& o) override;

  // Stops every sender (demotion, promotion: sub-replicas must resync, Redis disconnectSlaves; shutdown).
  void DropAllReplicas();

  std::string replid() const;
  Counters counters() const;
  std::vector<ReplicaInfo> Replicas() const;
  size_t pending_count() const;

 private:
  struct Pending {
    uint64_t next_seq = 0;
    ReplicaPeer peer;
    std::chrono::steady_clock::time_point created;
  };

  void TickLoop();
  void Reap();
  std::chrono::milliseconds Timeout() const;

  Storage& storage_;
  ReplLog& log_;
  Options options_;

  mutable std::mutex mu_;
  std::condition_variable tick_cv_;
  ReplIdentity identity_;                             // guarded by mu_
  std::map<uint64_t, Pending> pending_;               // guarded by mu_
  uint64_t next_pending_id_ = 1;                      // guarded by mu_
  std::vector<std::unique_ptr<ReplSender>> senders_;  // guarded by mu_
  bool shut_down_ = false;                            // guarded by mu_

  std::mutex shutdown_mu_;  // two Shutdown() calls join the tick thread once
  std::atomic<uint64_t> sync_full_{0};
  std::atomic<uint64_t> sync_partial_ok_{0};
  std::atomic<uint64_t> sync_partial_err_{0};
  std::thread tick_;
};

}  // namespace tideskv::repl
