#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "encoding/meta_codec.h"
#include "server/session.h"

// Blocked clients (M4 plan F4). Lock order: BlockedClient::mu_ -> BlockingRegistry::mu_ -> Engine. The
// registry never calls into a BlockedClient while holding its own mutex.
namespace tideskv {

class BlockingRegistry;

// One parked command. Created on its connection's worker thread; served, cancelled or unblocked from any
// thread. The wait ends exactly once, and ending it removes the client from the registry.
class BlockedClient {
 public:
  using Clock = std::chrono::steady_clock;
  enum class ServeResult : uint8_t { kServed, kStillBlocked, kGone };
  // Re-runs the command: its reply bytes, or nullopt when it would block again.
  using Attempt = std::function<std::optional<std::string>()>;

  // `session` is a copy of the connection's session at the time it blocked (protocol, db, link ...).
  BlockedClient(BlockingRegistry& registry, Session session, std::vector<std::string> argv,
                encoding::ValueType type, std::vector<std::string> keys,
                std::optional<Clock::time_point> deadline);
  BlockedClient(const BlockedClient&) = delete;
  BlockedClient& operator=(const BlockedClient&) = delete;

  // While waiting, runs `attempt` under this client's lock. A reply ends the wait and is delivered
  // through the link; nullopt keeps waiting. kGone if the wait already ended.
  ServeResult TryServe(const Attempt& attempt);
  // Ends the wait without a reply (timeout, disconnect). True if this call ended it: the caller replies.
  bool Cancel();
  // Ends the wait with `reply` delivered through the link (CLIENT UNBLOCK). True if this call ended it.
  bool Unblock(std::string reply);

  uint64_t client_id() const { return session_.id; }
  uint8_t db() const { return session_.db; }
  encoding::ValueType type() const { return type_; }
  const std::vector<std::string>& keys() const { return keys_; }  // deduplicated, argument order
  const std::vector<std::string>& argv() const { return argv_; }
  const Session& session() const { return session_; }
  std::optional<Clock::time_point> deadline() const { return deadline_; }

 private:
  void EndLocked();
  void Deliver(std::string reply);

  BlockingRegistry& registry_;
  const Session session_;
  const std::vector<std::string> argv_;
  const encoding::ValueType type_;
  const std::vector<std::string> keys_;
  const std::optional<Clock::time_point> deadline_;
  std::mutex mu_;
  bool done_ = false;  // guarded by mu_
};

class BlockingRegistry {
 public:
  // A client id is registered at most once. Adding an id that is already registered (a caller bug: a
  // connection blocks once at a time) replaces the old entry in every index, so the registry stays
  // consistent and the connection's current wait is the one that can be served; the old wait is
  // orphaned (its later Remove is a no-op) and ends only by its own timeout or disconnect.
  void Add(const std::shared_ptr<BlockedClient>& client);
  void Remove(const BlockedClient& client);  // idempotent
  // Cheap when nobody is blocked anywhere (one atomic load).
  bool HasWaiters(uint8_t db, std::string_view key) const;
  // The clients blocked on (db, key), in the order they blocked.
  std::vector<std::shared_ptr<BlockedClient>> WaitersOn(uint8_t db, std::string_view key) const;
  std::shared_ptr<BlockedClient> Find(uint64_t client_id) const;
  size_t blocked_clients() const { return count_.load(); }
  size_t blocking_keys() const;  // distinct (db, key) with at least one waiter

 private:
  void EraseKeysLocked(const BlockedClient& client);  // drops client from by_key_ (mu_ held)

  mutable std::mutex mu_;
  std::map<std::string, std::list<std::shared_ptr<BlockedClient>>> by_key_;  // [db byte][key] -> FIFO
  std::unordered_map<uint64_t, std::shared_ptr<BlockedClient>> by_client_;
  std::atomic<size_t> count_{0};
};

}  // namespace tideskv
