#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "exec/write_observer.h"

namespace tideskv {

// One connection's WATCH state (M5a plan G2). keys() belongs to the connection's thread;
// the dirty flag (Redis CLIENT_DIRTY_CAS) is set from any writer's thread.
class WatchClient {
 public:
  bool dirty() const { return dirty_.load(std::memory_order_acquire); }
  void MarkDirty() { dirty_.store(true, std::memory_order_release); }
  const std::vector<DbKey>& keys() const { return keys_; }

 private:
  friend class WatchRegistry;
  std::atomic<bool> dirty_{false};
  std::vector<DbKey> keys_;  // no duplicates
};

// Who watches which (db, key). Sharded by key; the Engine reports committed writes here.
// Lock order: Engine gate -> key stripes -> shard mutex. Never calls the Engine.
class WatchRegistry final : public WriteObserver {
 public:
  // Owner thread. Watches (db, key) unless the client already does or is dirty (Redis then
  // registers nothing). expired: the key exists but is logically expired now (wk->expired).
  // Returns true if this call registered the key (false: already watched, or the client is dirty).
  bool Watch(const std::shared_ptr<WatchClient>& client, uint8_t db, std::string_view key, bool expired);
  // Owner thread, after Watch() returned true and the key was read: records that it was already
  // expired at WATCH. No-op if the entry is gone or the client is dirty (a write landed in between).
  void MarkExpiredAtWatch(const WatchClient& client, uint8_t db, std::string_view key);
  // Owner thread. Drops one watch (a WATCH that failed after registering it); the dirty flag is kept.
  void Unwatch(WatchClient& client, uint8_t db, std::string_view key);
  // Owner thread. Drops every watch and clears the dirty flag (UNWATCH, EXEC, DISCARD, RESET, disconnect).
  void UnwatchAll(WatchClient& client);
  // Owner thread. The watched keys whose expired-at-watch flag is clear: EXEC checks them for expiry.
  std::vector<DbKey> KeysWatchedLive(const WatchClient& client) const;
  size_t size() const { return entries_.load(); }  // registered (client, key) pairs

  bool Idle() const override { return entries_.load() == 0; }
  void OnModified(const std::vector<DbKey>& keys, const CommittedViewFn& committed) override;
  std::vector<std::string> ExistingWatched(uint8_t db, ReadView& view) override;

 private:
  struct Entry {
    std::shared_ptr<WatchClient> client;
    bool expired = false;
  };
  struct Shard {
    mutable std::mutex mu;
    std::unordered_map<std::string, std::vector<Entry>> by_key;  // [db byte][user key]
  };
  static constexpr size_t kShards = 64;
  static std::string MapKey(uint8_t db, std::string_view key);
  Shard& ShardFor(const std::string& map_key);
  const Shard& ShardFor(const std::string& map_key) const;
  void RemoveEntries(const WatchClient& client, uint8_t db, std::string_view key);  // the registry side

  std::array<Shard, kShards> shards_;
  std::atomic<size_t> entries_{0};  // seq_cst: see Idle() and Watch()
};

}  // namespace tideskv
