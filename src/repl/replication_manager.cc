#include "repl/replication_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

#include "common/string_util.h"
#include "types/errors.h"

namespace tideskv::repl {
namespace {

using namespace std::chrono_literals;

constexpr auto kTick = 1s;
constexpr auto kDefaultTimeout = 60s;

}  // namespace

ReplicationManager::ReplicationManager(Storage& storage, ReplLog& log, ReplIdentity identity)
    : ReplicationManager(storage, log, std::move(identity), Options{}) {}

ReplicationManager::ReplicationManager(Storage& storage, ReplLog& log, ReplIdentity identity, Options options)
    : storage_(storage), log_(log), options_(std::move(options)), identity_(std::move(identity)) {}

ReplicationManager::~ReplicationManager() { Shutdown(); }

void ReplicationManager::Start() {
  std::lock_guard lock(mu_);
  if (shut_down_ || tick_.joinable()) return;
  tick_ = std::thread([this] { TickLoop(); });
}

void ReplicationManager::Shutdown() {
  {
    std::lock_guard lock(mu_);
    shut_down_ = true;
    pending_.clear();
  }
  tick_cv_.notify_all();
  {
    std::lock_guard lock(shutdown_mu_);
    if (tick_.joinable()) tick_.join();
  }
  {
    std::lock_guard role_lock(role_mu_);  // waits out a role change in progress, which then starts nothing
    if (auto client = TakeClient()) client->Stop();
  }
  DropAllReplicas();
}

std::chrono::milliseconds ReplicationManager::Timeout() const {
  return options_.repl_timeout ? options_.repl_timeout() : std::chrono::milliseconds(kDefaultTimeout);
}

PsyncResult ReplicationManager::HandlePsync(const PsyncRequest& req, const ReplicaPeer& peer) {
  const LogBounds bounds{log_.first_retained(), log_.last()};
  std::lock_guard lock(mu_);
  PsyncResult r;
  r.replid = identity_.replid;
  r.seq = bounds.last;
  r.decision = DecidePsync(identity_, bounds, req);
  if (r.decision == PsyncDecision::kContinue) {
    sync_partial_ok_.fetch_add(1, std::memory_order_relaxed);
    r.pending_id = next_pending_id_++;
    pending_[r.pending_id] = Pending{req.next_seq, peer, std::chrono::steady_clock::now()};
    return r;
  }
  sync_full_.fetch_add(1, std::memory_order_relaxed);  // M6b sends the data; M6a closes the link
  if (req.replid != "?") sync_partial_err_.fetch_add(1, std::memory_order_relaxed);
  return r;
}

void ReplicationManager::AttachReplica(uint64_t pending_id, int fd, std::string unread) {
  Fd owned(fd);  // closed on every refusal below
  std::lock_guard lock(mu_);
  if (shut_down_) return;
  const auto it = pending_.find(pending_id);
  if (it == pending_.end()) {
    spdlog::warn("replica hand-off {} refused: no pending PSYNC (expired?)", pending_id);
    return;
  }
  const Pending p = std::move(it->second);
  pending_.erase(it);
  ReplSender::Callbacks cb;
  cb.repl_timeout = [this] { return Timeout(); };
  auto sender = std::make_unique<ReplSender>(storage_, log_, std::move(owned), p.next_seq, std::move(cb));
  sender->SetPeer(p.peer.ip, p.peer.listening_port);
  sender->SetUnread(std::move(unread));
  sender->Start();  // spawns the thread; never blocks
  spdlog::info("replica {}:{} attached, streaming from seq {}", p.peer.ip, p.peer.listening_port, p.next_seq);
  senders_.push_back(std::move(sender));
}

void ReplicationManager::OnReplconf(Session& /*s*/, const ReplconfOptions& /*o*/) {
  // Master half: the command already recorded listening-port / ip-address in the session, and PSYNC
  // copies them into the pending entry. GETACK on a replica arrives with Task 12.
}

// D7, D13, D14: fence, stop the reaper, unblock and close blocked clients, drop sub-replicas, swap the
// client. Joins threads (reaper, old client, senders) but never waits for the network.
bool ReplicationManager::BecomeReplica(const std::string& host, uint16_t port) {
  std::lock_guard role_lock(role_mu_);
  if (SameMaster(host, port)) return false;
  {
    std::lock_guard lock(mu_);
    if (shut_down_) return true;
  }
  const RoleHooks& hooks = options_.role;
  log_.FenceLocalWrites(true);  // a barrier: once it returns no local commit can take a seq (CF1)
  std::shared_ptr<ReplClient> old;
  {
    std::lock_guard lock(mu_);  // Role() sees the role, the master and (no) client change together
    old = std::exchange(client_, nullptr);
    master_host_ = host;
    master_port_ = port;
    role_.store(ReplRole::kReplica, std::memory_order_release);
  }
  if (hooks.stop_reaper) hooks.stop_reaper();
  // A blocking command dispatched before this point either registered already (and is ended here) or
  // registers later and is refused: DisconnectAll moves the registry to a new epoch (fix round 1).
  if (hooks.disconnect_blocked) hooks.disconnect_blocked(kErrUnblockedRoleChange);
  DropAllReplicas();
  if (old) old->Stop();  // shutdown + join: the only committer is gone
  SetReplicaOf(host + " " + std::to_string(port));
  StartClient(host, port);
  spdlog::info("REPLICAOF {}:{}: this node is now a replica", host, port);
  return true;
}

void ReplicationManager::StartClient(const std::string& host, uint16_t port) {
  std::shared_ptr<ReplClient> client = MakeClient();
  if (!client) return;
  {
    std::lock_guard lock(mu_);
    client_ = client;
  }
  client->Start(host, port);
}

// REPLICAOF NO ONE (D7): stop the client, then reload the oid hwm and persist the promoted identity on
// this thread. With the client joined nothing applies concurrently, so this thread is the replica's only
// committer, and the fence is still up. If either step fails the node stays a fenced replica of the same
// master (its client restarted) and the error is returned; only then does it unfence and act as a master.
Status ReplicationManager::BecomeMaster() {
  std::lock_guard role_lock(role_mu_);
  if (role() == ReplRole::kMaster) return Status::Ok();
  std::string host;
  uint16_t port = 0;
  {
    std::lock_guard lock(mu_);
    if (shut_down_) return Status::Ok();
    host = master_host_;
    port = master_port_;
  }
  if (auto old = TakeClient()) old->Stop();
  const uint64_t last = log_.last();
  const ReplIdentity promoted = Promote(identity(), last);
  Status st = options_.role.reinit_oids ? options_.role.reinit_oids() : Status::Ok();  // CF7
  if (st.ok()) st = PersistIdentity(storage_, promoted);
  if (!st.ok()) {
    spdlog::error("REPLICAOF NO ONE failed, still a replica of {}:{}: {}", host, port, st.ToString());
    StartClient(host, port);
    return st;
  }
  log_.FenceLocalWrites(false);
  {
    std::lock_guard lock(mu_);
    identity_ = promoted;
    master_host_.clear();
    master_port_ = 0;
    role_.store(ReplRole::kMaster, std::memory_order_release);
  }
  if (options_.role.start_reaper) options_.role.start_reaper();
  DropAllReplicas();  // sub-replicas must learn the new replid (Redis disconnectSlaves)
  SetReplicaOf("");
  spdlog::info("REPLICAOF NO ONE: this node is now a master, replication id {} from seq {}", promoted.replid,
               last + 1);
  return Status::Ok();
}

RoleInfo ReplicationManager::Role() const {
  RoleInfo r;
  r.offset = log_.last();
  std::shared_ptr<ReplClient> client;
  {
    std::lock_guard lock(mu_);
    r.role = role();
    r.host = master_host_;
    r.port = master_port_;
    client = client_;
  }
  if (r.role == ReplRole::kMaster) {
    r.replicas = Replicas();
    return r;
  }
  r.state = client ? client->state() : ReplicaLinkState::kConnect;
  r.offset_or_minus1 = r.state == ReplicaLinkState::kConnected ? static_cast<int64_t>(r.offset) : -1;
  return r;
}

ReplicationManager::LinkTimes ReplicationManager::Link() const {
  std::shared_ptr<ReplClient> client;
  {
    std::lock_guard lock(mu_);
    client = client_;
  }
  if (!client) return {};
  return {client->last_io_ms(), client->link_down_since_ms()};
}

bool ReplicationManager::SameMaster(const std::string& host, uint16_t port) const {
  std::lock_guard lock(mu_);
  return role() == ReplRole::kReplica && EqualsIgnoreCase(host, master_host_) && port == master_port_;
}

std::shared_ptr<ReplClient> ReplicationManager::TakeClient() {
  std::lock_guard lock(mu_);
  return std::exchange(client_, nullptr);
}

std::shared_ptr<ReplClient> ReplicationManager::MakeClient() {
  if (options_.role.engine == nullptr) return nullptr;  // no engine to apply through (unit tests)
  ReplClient::Callbacks cb;
  cb.identity = [this] { return identity(); };
  cb.persist_identity = [this](const ReplIdentity& id) { return PersistAndPublish(id); };
  cb.trim_backlog = options_.role.trim_backlog;
  ReplClient::ConfigSnapshotFn config = options_.role.config;
  if (!config) config = [] { return ConfigValues{}; };
  return std::make_shared<ReplClient>(*options_.role.engine, log_, std::move(config), std::move(cb),
                                      options_.client);
}

Status ReplicationManager::PersistAndPublish(const ReplIdentity& id) {
  if (Status st = PersistIdentity(storage_, id); !st.ok()) return st;
  std::lock_guard lock(mu_);
  identity_ = id;
  return Status::Ok();
}

void ReplicationManager::SetReplicaOf(const std::string& value) {
  if (options_.role.set_replicaof) options_.role.set_replicaof(value);
}

void ReplicationManager::DropAllReplicas() {
  std::vector<std::unique_ptr<ReplSender>> doomed;
  {
    std::lock_guard lock(mu_);
    doomed.swap(senders_);
  }
  for (auto& s : doomed) s->Stop();  // outside mu_: joins
}

uint64_t ReplicationManager::OldestNeeded() const {
  std::lock_guard lock(mu_);
  uint64_t oldest = kNoReplicaNeeds;
  for (const auto& s : senders_) {
    if (s->alive()) oldest = std::min(oldest, s->oldest_needed());
  }
  for (const auto& [id, p] : pending_) oldest = std::min(oldest, std::max<uint64_t>(p.next_seq, 1));
  return oldest;
}

void ReplicationManager::DropReplicasNeeding(uint64_t up_to) {
  std::vector<std::unique_ptr<ReplSender>> doomed;
  {
    std::lock_guard lock(mu_);
    const auto split = std::stable_partition(senders_.begin(), senders_.end(),
                                             [&](const auto& s) { return s->oldest_needed() > up_to; });
    std::move(split, senders_.end(), std::back_inserter(doomed));
    senders_.erase(split, senders_.end());
    std::erase_if(pending_,
                  [&](const auto& kv) { return std::max<uint64_t>(kv.second.next_seq, 1) <= up_to; });
  }
  for (auto& s : doomed) {  // outside mu_: joins
    spdlog::warn(
        "dropping replica {}:{}: it needs seq {}, but the backlog is trimmed through {}; it needs a "
        "full resynchronisation",
        s->peer_ip(), s->listening_port(), s->oldest_needed(), up_to);
    s->Stop();
  }
}

void ReplicationManager::TickLoop() {
  std::unique_lock lock(mu_);
  while (!shut_down_) {
    if (tick_cv_.wait_for(lock, kTick, [this] { return shut_down_; })) break;
    lock.unlock();
    Reap();
    lock.lock();
  }
}

// Joins senders whose link dropped and forgets hand-offs that never completed within repl-timeout
// (the client went away before its +CONTINUE was flushed).
void ReplicationManager::Reap() {
  const auto timeout = Timeout();
  const auto now = std::chrono::steady_clock::now();
  std::vector<std::unique_ptr<ReplSender>> dead;
  {
    std::lock_guard lock(mu_);
    const auto split =
        std::stable_partition(senders_.begin(), senders_.end(), [](const auto& s) { return s->alive(); });
    std::move(split, senders_.end(), std::back_inserter(dead));
    senders_.erase(split, senders_.end());
    std::erase_if(pending_, [&](const auto& kv) { return now - kv.second.created > timeout; });
  }
  for (auto& s : dead) s->Stop();
}

std::string ReplicationManager::replid() const {
  std::lock_guard lock(mu_);
  return identity_.replid;
}

ReplIdentity ReplicationManager::identity() const {
  std::lock_guard lock(mu_);
  return identity_;
}

ReplicationManager::Counters ReplicationManager::counters() const {
  Counters c;
  c.sync_full = sync_full_.load(std::memory_order_relaxed);
  c.sync_partial_ok = sync_partial_ok_.load(std::memory_order_relaxed);
  c.sync_partial_err = sync_partial_err_.load(std::memory_order_relaxed);
  std::lock_guard lock(mu_);
  c.connected_replicas = static_cast<uint64_t>(
      std::count_if(senders_.begin(), senders_.end(), [](const auto& s) { return s->alive(); }));
  return c;
}

std::vector<ReplicationManager::ReplicaInfo> ReplicationManager::Replicas() const {
  std::lock_guard lock(mu_);
  std::vector<ReplicaInfo> out;
  out.reserve(senders_.size());
  for (const auto& s : senders_) {
    out.push_back({s->peer_ip(), s->listening_port(), s->acked(), s->last_ack_ms(), s->alive()});
  }
  return out;
}

size_t ReplicationManager::pending_count() const {
  std::lock_guard lock(mu_);
  return pending_.size();
}

}  // namespace tideskv::repl
