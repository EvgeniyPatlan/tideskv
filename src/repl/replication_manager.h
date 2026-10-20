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
#include <string_view>
#include <thread>
#include <vector>

#include "common/status.h"
#include "exec/engine.h"
#include "exec/repl_log.h"
#include "repl/backlog_trimmer.h"
#include "repl/repl_client.h"
#include "repl/repl_sender.h"
#include "repl/repl_state.h"
#include "repl/replication.h"
#include "repl/wait_list.h"
#include "server/config.h"
#include "storage/storage.h"

// Owns the node's replication role and its links (M6a D7-D10, D13, D14). The master half: PSYNC, the
// socket hand-off and one ReplSender thread per replica. The replica half: one ReplClient thread, and the
// role changes REPLICAOF host port (BecomeReplica) and REPLICAOF NO ONE (BecomeMaster).
//
// Threads: a maintenance thread ticks every second to reap dead senders and expire pending hand-offs
// (the Task 14 BacklogTrimmer is a separate thread: it asks OldestNeeded and calls DropReplicasNeeding). mu_
// guards the identity, the pending table, the sender list, the master address and the client pointer; it is
// never held while calling the Engine, while blocked on a socket, or while joining a thread. role_mu_
// serialises role changes (and Shutdown's stop of the client); it is held across those joins, so nothing
// a joined thread does may take it.
namespace tideskv::repl {

class ReplicationManager final : public Replication {
 public:
  // What a role change does outside replication (D6/CF7, D13, D14, D15). Every hook is optional.
  struct RoleHooks {
    Engine* engine = nullptr;              // the replica applies through it (required by BecomeReplica)
    std::function<ConfigValues()> config;  // the ReplClient's settings; null = defaults
    std::function<void()> stop_reaper;     // D13
    std::function<void()> start_reaper;    // D13
    std::function<void(std::string_view error)> disconnect_blocked;  // D14: every blocked client
    std::function<Status()> reinit_oids;                             // CF7: OidAllocator::Init
    std::function<void(const std::string& value)> set_replicaof;     // D15: "host port", or "" for none
    std::function<Status()> trim_backlog;  // D18: the replica's trim, run by its client between batches
  };
  struct Options {
    std::function<std::chrono::milliseconds()> repl_timeout;  // null = 60 s
    // F4: live replica links count against maxclients together with the server's client connections.
    // A PSYNC caller, and a hand-off in flight, is still a connection: its sender is alive before the
    // connection goes, so a link is never uncounted (and counted twice only for that moment). Either
    // null = no limit.
    std::function<uint64_t()> maxclients;
    std::function<uint64_t()> connected_clients;
    // F5: this node's log does not describe its data (LoadOrDetectBaselineUnknown). As a master it never
    // continues a "?" request; as a replica its client never sends "?". The initial value: a full sync
    // clears it (M6b D5).
    bool baseline_unknown = false;
    RoleHooks role;
    ReplClient::Options client;
  };
  struct Counters {
    uint64_t sync_full = 0;         // FULLRESYNC replies
    uint64_t sync_partial_ok = 0;   // CONTINUE replies
    uint64_t sync_partial_err = 0;  // FULLRESYNC to a request that named a replid
    uint64_t connected_replicas = 0;
  };
  using ReplicaInfo = tideskv::ReplicaInfo;

  // Two constructors rather than a defaulted Options: GCC cannot default-construct a nested aggregate
  // with member initializers inside its enclosing class.
  ReplicationManager(Storage& storage, ReplLog& log, ReplIdentity identity);
  ReplicationManager(Storage& storage, ReplLog& log, ReplIdentity identity, Options options);
  ~ReplicationManager() override;  // Shutdown()
  ReplicationManager(const ReplicationManager&) = delete;
  ReplicationManager& operator=(const ReplicationManager&) = delete;

  void Start();  // the maintenance thread
  // Idempotent: stops the tick, the replica client and every sender; later hand-offs and role changes
  // are refused.
  void Shutdown();

  ReplRole role() const override { return role_.load(std::memory_order_acquire); }
  PsyncResult HandlePsync(const PsyncRequest& req, const ReplicaPeer& peer) override;
  void AttachReplica(uint64_t pending_id, int fd, std::string unread) override;
  void OnReplconf(Session& s, const ReplconfOptions& o) override;
  bool BecomeReplica(const std::string& host, uint16_t port) override;
  Status BecomeMaster() override;
  RoleInfo Role() const override;
  bool StaleRefusing() const override;
  bool GoodReplicasOk() const override;
  // Online, alive links whose last ACK is within min-replicas-max-lag seconds.
  int GoodReplicaCount() const;
  int64_t CountAcked(uint64_t seq) const override;
  void AddWaiter(std::shared_ptr<BlockedClient> client, uint64_t seq, int64_t numreplicas) override;
  // Stores the config values that matter to replication. Only the single-threaded startup call and
  // RefreshConfig may use it: concurrent callers could store in the wrong order.
  void OnConfig(const ConfigValues& v);
  // The Config::SetOnChange path (CF-M6b-1): hook calls from concurrent Set/SetMany are not ordered, so
  // the snapshot is taken and stored under config_mu_. The last caller to get the lock snapshots after
  // every completed Set, so the stored values always end equal to Config's.
  void RefreshConfig(const std::function<ConfigValues()>& snapshot);

  // Stops every sender (demotion, promotion: sub-replicas must resync, Redis disconnectSlaves; shutdown).
  void DropAllReplicas();
  // Backlog trimming (D18). The oldest seq an attached replica or a pending hand-off still needs (a
  // sender: max(acked, its start - 1) + 1); kNoReplicaNeeds when there is none.
  uint64_t OldestNeeded() const;
  // Stops the senders, and forgets the hand-offs, that need a seq <= up_to: they are beyond the backlog
  // limits and will get +FULLRESYNC (M6b) when they come back. Joins outside mu_, as DropAllReplicas.
  void DropReplicasNeeding(uint64_t up_to);

  // The replica link's I/O times (unix ms), for INFO. Defaults when there is no client.
  struct LinkTimes {
    int64_t last_io_ms = 0;           // 0 = never
    int64_t link_down_since_ms = -1;  // -1 = never up, 0 = up
  };
  LinkTimes Link() const;
  // A replica's full-sync progress, for INFO (M6b D11). Zeros without a client.
  struct SyncProgress {
    bool in_progress = false;  // the client is in kSync
    uint64_t read_bytes = 0;
    int64_t last_io_ms = 0;
    int64_t loading_start_ms = 0;
    uint64_t loaded_bytes = 0;
  };
  SyncProgress Sync() const;
  bool baseline_unknown() const { return baseline_unknown_.load(std::memory_order_acquire); }

  std::string replid() const;
  ReplIdentity identity() const;
  Counters counters() const;
  std::vector<ReplicaInfo> Replicas() const;
  size_t pending_count() const;

 private:
  struct Pending {
    uint64_t next_seq = 0;
    ReplicaPeer peer;
    std::chrono::steady_clock::time_point created;
    std::unique_ptr<SyncSnapshot> snapshot;  // FULLRESYNC: what the sender streams first (D2)
  };

  // PSYNC in two steps, so that ReplLog::ActivateAndSnapshot runs outside mu_ (CF-M6b-3).
  PsyncResult Decide(const PsyncRequest& req, const ReplicaPeer& peer);
  // The PSYNC refusals (shutdown, F8, F4) as their error text; empty = none. RefusalLocked requires mu_.
  std::string Refusal();
  std::string RefusalLocked(uint64_t maxclients) const;
  PsyncResult RegisterFullSync(PsyncResult r, SyncSnapshot snap, const PsyncRequest& req,
                               const ReplicaPeer& peer);
  // Moves the hand-offs matching pred out of pending_ (requires mu_). The caller drops them after
  // releasing mu_: they may hold a storage snapshot.
  std::vector<Pending> TakePendingLocked(const std::function<bool(const Pending&)>& pred);

  uint64_t LiveSendersLocked() const;        // requires mu_
  std::vector<uint64_t> OnlineAcks() const;  // the alive, online senders' ACKs (takes mu_)
  // A sender's on_ack (its thread) and AddWaiter: answers the WAIT clients the ACKs now satisfy, outside
  // every manager lock (they take BlockedClient::mu_).
  void OnAck();
  void TickLoop();
  void Reap();
  std::chrono::milliseconds Timeout() const;
  Status RefuseHalfLoaded(bool stopped_mid_load) const;  // D22: kErrLoading for a half-loaded node
  std::shared_ptr<ReplClient> TakeClient();  // detaches the client (under mu_); the caller stops it
  std::shared_ptr<ReplClient> MakeClient();
  void StartClient(const std::string& host, uint16_t port);  // a new client, published under mu_
  Status PersistAndPublish(const ReplIdentity& id);          // the client thread's persist_identity
  bool SameMaster(const std::string& host, uint16_t port) const;
  void SetReplicaOf(const std::string& value);

  Storage& storage_;
  ReplLog& log_;
  Options options_;

  std::mutex config_mu_;  // RefreshConfig only; never held with another lock
  mutable std::mutex mu_;
  std::condition_variable tick_cv_;
  ReplIdentity identity_;                             // guarded by mu_
  std::map<uint64_t, Pending> pending_;               // guarded by mu_
  uint64_t next_pending_id_ = 1;                      // guarded by mu_
  std::vector<std::unique_ptr<ReplSender>> senders_;  // guarded by mu_
  bool shut_down_ = false;                            // guarded by mu_
  std::string master_host_;                           // guarded by mu_; set while a replica
  uint16_t master_port_ = 0;                          // guarded by mu_
  std::shared_ptr<ReplClient> client_;                // guarded by mu_; shared so Role() may read it
  WaitList waits_;                                    // WAIT clients (D9); its own lock, never with mu_
  std::atomic<ReplRole> role_{ReplRole::kMaster};
  std::atomic<bool> serve_stale_{true};                                   // replica-serve-stale-data
  std::atomic<int> min_replicas_to_write_{0};                             // min-replicas-to-write (D12)
  std::atomic<int> min_replicas_max_lag_{10};                             // min-replicas-max-lag, seconds
  std::atomic<int> key_save_delay_us_{0};                                 // rdb-key-save-delay (D14)
  std::atomic<ReplicaLinkState> link_state_{ReplicaLinkState::kConnect};  // mirrored from the client
  std::atomic<bool> baseline_unknown_;  // F5: from Options; a replica's full sync clears it (M6b D5)

  std::mutex role_mu_;  // serialises BecomeReplica / BecomeMaster / Shutdown's client stop

  std::mutex shutdown_mu_;  // two Shutdown() calls join the tick thread once
  std::atomic<uint64_t> sync_full_{0};
  std::atomic<uint64_t> sync_partial_ok_{0};
  std::atomic<uint64_t> sync_partial_err_{0};
  std::thread tick_;
};

}  // namespace tideskv::repl
