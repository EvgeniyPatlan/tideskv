#include "server/blocking.h"

#include <algorithm>
#include <utility>

namespace tideskv {
namespace {

std::string KeyOf(uint8_t db, std::string_view key) {
  std::string out(1, static_cast<char>(db));
  out.append(key);
  return out;
}

std::vector<std::string> Dedupe(std::vector<std::string> keys) {
  std::vector<std::string> out;
  for (std::string& k : keys) {
    if (std::find(out.begin(), out.end(), k) == out.end()) out.push_back(std::move(k));
  }
  return out;
}

}  // namespace

BlockedClient::BlockedClient(BlockingRegistry& registry, Session session, std::vector<std::string> argv,
                             encoding::ValueType type, std::vector<std::string> keys,
                             std::optional<Clock::time_point> deadline)
    : registry_(registry),
      session_(std::move(session)),
      argv_(std::move(argv)),
      type_(type),
      keys_(Dedupe(std::move(keys))),
      deadline_(deadline) {}

BlockedClient::ServeResult BlockedClient::TryServe(const Attempt& attempt) {
  std::lock_guard lock(mu_);
  if (done_) return ServeResult::kGone;
  std::optional<std::string> reply = attempt();
  if (!reply) return ServeResult::kStillBlocked;
  EndLocked();
  Deliver(std::move(*reply));
  return ServeResult::kServed;
}

bool BlockedClient::Cancel() {
  std::lock_guard lock(mu_);
  if (done_) return false;
  EndLocked();
  return true;
}

bool BlockedClient::Unblock(std::string reply) {
  std::lock_guard lock(mu_);
  if (done_) return false;
  EndLocked();
  Deliver(std::move(reply));
  return true;
}

void BlockedClient::EndLocked() {
  done_ = true;
  registry_.Remove(*this);
}

void BlockedClient::Deliver(std::string reply) {
  if (auto link = session_.link.lock()) link->Unblock(std::move(reply));
}

void BlockingRegistry::Add(const std::shared_ptr<BlockedClient>& client) {
  std::lock_guard lock(mu_);
  auto& slot = by_client_[client->client_id()];
  if (slot) {
    EraseKeysLocked(*slot);  // a duplicate id replaces the old entry (see the header)
  } else {
    count_.fetch_add(1);
  }
  slot = client;
  for (const std::string& k : client->keys()) by_key_[KeyOf(client->db(), k)].push_back(client);
}

void BlockingRegistry::Remove(const BlockedClient& client) {
  std::lock_guard lock(mu_);
  const auto it = by_client_.find(client.client_id());
  if (it == by_client_.end() || it->second.get() != &client) return;
  by_client_.erase(it);
  EraseKeysLocked(client);
  count_.fetch_sub(1);
}

void BlockingRegistry::EraseKeysLocked(const BlockedClient& client) {
  for (const std::string& k : client.keys()) {
    const auto kit = by_key_.find(KeyOf(client.db(), k));
    if (kit == by_key_.end()) continue;
    kit->second.remove_if([&](const std::shared_ptr<BlockedClient>& p) { return p.get() == &client; });
    if (kit->second.empty()) by_key_.erase(kit);
  }
}

bool BlockingRegistry::HasWaiters(uint8_t db, std::string_view key) const {
  if (count_.load() == 0) return false;
  std::lock_guard lock(mu_);
  return by_key_.contains(KeyOf(db, key));
}

std::vector<std::shared_ptr<BlockedClient>> BlockingRegistry::WaitersOn(uint8_t db,
                                                                        std::string_view key) const {
  std::lock_guard lock(mu_);
  const auto it = by_key_.find(KeyOf(db, key));
  if (it == by_key_.end()) return {};
  return {it->second.begin(), it->second.end()};
}

std::shared_ptr<BlockedClient> BlockingRegistry::Find(uint64_t client_id) const {
  std::lock_guard lock(mu_);
  const auto it = by_client_.find(client_id);
  return it == by_client_.end() ? nullptr : it->second;
}

size_t BlockingRegistry::blocking_keys() const {
  std::lock_guard lock(mu_);
  return by_key_.size();
}

}  // namespace tideskv
