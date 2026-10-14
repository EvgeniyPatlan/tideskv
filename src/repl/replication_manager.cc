#include "repl/replication_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

namespace tideskv::repl {
namespace {

using namespace std::chrono_literals;

constexpr auto kTick = 1s;
constexpr auto kDefaultTimeout = 60s;

}  // namespace

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

void ReplicationManager::DropAllReplicas() {
  std::vector<std::unique_ptr<ReplSender>> doomed;
  {
    std::lock_guard lock(mu_);
    doomed.swap(senders_);
  }
  for (auto& s : doomed) s->Stop();  // outside mu_: joins
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
