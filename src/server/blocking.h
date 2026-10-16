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
  // `timeout_reply` builds the reply for a timeout or CLIENT UNBLOCK TIMEOUT (WAIT); none = a null array.
  BlockedClient(BlockingRegistry& registry, Session session, std::vector<std::string> argv,
                encoding::ValueType type, std::vector<std::string> keys,
                std::optional<Clock::time_point> deadline, std::function<std::string()> timeout_reply = {});
  BlockedClient(const BlockedClient&) = delete;
  BlockedClient& operator=(const BlockedClient&) = delete;

  // While waiting, runs `attempt` under this client's lock. A reply ends the wait and is delivered
  // through the link; nullopt keeps waiting. kGone if the wait already ended.
  ServeResult TryServe(const Attempt& attempt);
  // Ends the wait without a reply (timeout, disconnect). True if this call ended it: the caller replies.
  bool Cancel();
  // Ends the wait with `reply` delivered through the link (CLIENT UNBLOCK). True if this call ended it.
  bool Unblock(std::string reply);
  // Like Unblock, and the connection closes after the reply (M6a D14). True if this call ended it.
  bool UnblockAndClose(std::string reply);

  uint64_t client_id() const { return session_.id; }
  uint8_t db() const { return session_.db; }
  encoding::ValueType type() const { return type_; }
  const std::vector<std::string>& keys() const { return keys_; }  // deduplicated, argument order
  const std::vector<std::string>& argv() const { return argv_; }
  const Session& session() const { return session_; }
  std::optional<Clock::time_point> deadline() const { return deadline_; }
  // The encoded timeout reply; empty when the command has none (the caller replies a null array).
  std::string TimeoutReply() const;
  // The seq TryServe's attempt committed on the serving thread (0: it wrote nothing). Read by the client's
  // connection once the served reply arrives, so its next WAIT counts that write (M6b D9).
  uint64_t served_seq() const { return served_seq_.load(std::memory_order_acquire); }
  bool ended() const;  // the wait ended (served, timed out, unblocked or cancelled)

 private:
  void EndLocked();
  void Deliver(std::string reply, bool close_after);

  BlockingRegistry& registry_;
  const Session session_;
  const std::vector<std::string> argv_;
  const encoding::ValueType type_;
  const std::vector<std::string> keys_;
  const std::optional<Clock::time_point> deadline_;
  const std::function<std::string()> timeout_reply_;
  std::atomic<uint64_t> served_seq_{0};
  mutable std::mutex mu_;
  bool done_ = false;  // guarded by mu_
};

class BlockingRegistry {
 public:
  // A client id is registered at most once. Adding an id that is already registered (a caller bug: a
  // connection blocks once at a time) replaces the old entry in every index, so the registry stays
  // consistent and the connection's current wait is the one that can be served; the old wait is
  // orphaned (its later Remove is a no-op) and ends only by its own timeout or disconnect.
  void Add(const std::shared_ptr<BlockedClient>& client);
  // Add, unless a DisconnectAll ran since `epoch` was read (a blocking command dispatched before a
  // demotion must not start waiting after it; *close_after = true) or the redirect check refuses the wait
  // (a command routed under an older topology, M7a D16; *close_after = false). Then nothing is registered,
  // *refusal gets the reply bytes, and false is returned.
  bool AddIfCurrent(const std::shared_ptr<BlockedClient>& client, uint64_t epoch, std::string* refusal,
                    bool* close_after);
  uint64_t epoch() const { return epoch_.load(std::memory_order_acquire); }  // read before dispatch
  void Remove(const BlockedClient& client);                                  // idempotent
  // Cheap when nobody is blocked anywhere (one atomic load).
  bool HasWaiters(uint8_t db, std::string_view key) const;
  // The clients blocked on (db, key), in the order they blocked.
  std::vector<std::shared_ptr<BlockedClient>> WaitersOn(uint8_t db, std::string_view key) const;
  std::shared_ptr<BlockedClient> Find(uint64_t client_id) const;
  // Ends every wait with the error `error` (text without the leading '-') and closes each of those
  // connections after the reply (Redis disconnectAllBlockedClients; M6a D14), and starts a new epoch so
  // that AddIfCurrent refuses commands dispatched before it. Returns how many waits it ended.
  size_t DisconnectAll(std::string_view error);
  // M7a D16: the check every wait must pass (the cluster topology): the error that ends it, or nullopt.
  // Waits that fail it now end with its error (the connection stays open); AddIfCurrent refuses a later
  // wait that fails it; an empty Redirect removes the check. Called with mu_ held, so it must only read the
  // client's const state. Never blocks on another thread: it takes mu_, then each ended client's own mutex
  // (outside mu_), and delivers by posting to the connection. Returns how many waits it ended.
  using Redirect = std::function<std::optional<std::string>(const BlockedClient&)>;
  size_t SetRedirect(Redirect redirect);
  size_t blocked_clients() const { return count_.load(); }
  size_t blocking_keys() const;  // distinct (db, key) with at least one waiter

 private:
  void AddLocked(const std::shared_ptr<BlockedClient>& client);  // mu_ held
  void EraseKeysLocked(const BlockedClient& client);             // drops client from by_key_ (mu_ held)

  mutable std::mutex mu_;
  std::map<std::string, std::list<std::shared_ptr<BlockedClient>>> by_key_;  // [db byte][key] -> FIFO
  std::unordered_map<uint64_t, std::shared_ptr<BlockedClient>> by_client_;
  std::atomic<size_t> count_{0};
  std::atomic<uint64_t> epoch_{0};  // written under mu_
  std::string refusal_;             // guarded by mu_: the last DisconnectAll's reply
  Redirect redirect_;               // guarded by mu_
};

}  // namespace tideskv
