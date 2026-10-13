#include "server/watch.h"

#include <algorithm>

#include "encoding/key_codec.h"

namespace tideskv {
namespace {

// Physically present (expired or not). A read error counts as present: a spurious abort is safe.
bool MetaExists(ReadView& view, uint8_t db, std::string_view key) {
  auto r = view.Get(ColumnFamily::kMeta, encoding::MetaKeyFor(db, key));
  return r.ok() || !r.status().IsNotFound();
}

}  // namespace

std::string WatchRegistry::MapKey(uint8_t db, std::string_view key) {
  std::string out(1, static_cast<char>(db));
  out.append(key);
  return out;
}

WatchRegistry::Shard& WatchRegistry::ShardFor(const std::string& map_key) {
  return shards_[std::hash<std::string>{}(map_key) % kShards];
}

const WatchRegistry::Shard& WatchRegistry::ShardFor(const std::string& map_key) const {
  return shards_[std::hash<std::string>{}(map_key) % kShards];
}

bool WatchRegistry::Watch(const std::shared_ptr<WatchClient>& client, uint8_t db, std::string_view key,
                          bool expired) {
  if (client->dirty()) return false;
  const DbKey dk{db, std::string(key)};
  if (std::find(client->keys_.begin(), client->keys_.end(), dk) != client->keys_.end()) return false;
  const std::string mk = MapKey(db, key);
  Shard& s = ShardFor(mk);
  {
    std::lock_guard lock(s.mu);
    s.by_key[mk].push_back({client, expired});
    entries_.fetch_add(1);  // before the client reads the key: a writer that then sees 0 committed earlier
  }
  client->keys_.push_back(dk);
  return true;
}

void WatchRegistry::MarkExpiredAtWatch(const WatchClient& client, uint8_t db, std::string_view key) {
  const std::string mk = MapKey(db, key);
  Shard& s = ShardFor(mk);
  std::lock_guard lock(s.mu);
  if (client.dirty()) return;
  auto it = s.by_key.find(mk);
  if (it == s.by_key.end()) return;
  for (Entry& e : it->second) {
    if (e.client.get() == &client) e.expired = true;
  }
}

void WatchRegistry::UnwatchAll(WatchClient& client) {
  for (const DbKey& k : client.keys_) {
    const std::string mk = MapKey(k.db, k.key);
    Shard& s = ShardFor(mk);
    std::lock_guard lock(s.mu);
    auto it = s.by_key.find(mk);
    if (it == s.by_key.end()) continue;
    const auto removed = std::erase_if(it->second, [&](const Entry& e) { return e.client.get() == &client; });
    entries_.fetch_sub(removed);
    if (it->second.empty()) s.by_key.erase(it);
  }
  client.keys_.clear();
  client.dirty_.store(false, std::memory_order_release);
}

std::vector<DbKey> WatchRegistry::KeysWatchedLive(const WatchClient& client) const {
  std::vector<DbKey> out;
  for (const DbKey& k : client.keys_) {
    const std::string mk = MapKey(k.db, k.key);
    const Shard& s = ShardFor(mk);
    std::lock_guard lock(s.mu);
    auto it = s.by_key.find(mk);
    if (it == s.by_key.end()) continue;
    for (const Entry& e : it->second) {
      if (e.client.get() == &client && !e.expired) out.push_back(k);
    }
  }
  return out;
}

void WatchRegistry::OnModified(const std::vector<DbKey>& keys, const CommittedViewFn& committed) {
  for (const DbKey& k : keys) {
    const std::string mk = MapKey(k.db, k.key);
    Shard& s = ShardFor(mk);
    std::lock_guard lock(s.mu);
    auto it = s.by_key.find(mk);
    if (it == s.by_key.end()) continue;
    bool probed = false;
    bool exists = false;  // probed once, and only for a watcher whose key was expired at WATCH
    for (Entry& e : it->second) {
      if (e.expired) {
        if (!probed) {
          ReadView* view = committed();
          exists = view == nullptr || MetaExists(*view, k.db, k.key);
          probed = true;
        }
        if (!exists) {  // the stale key was deleted: no logical change (multi.c:393-402)
          e.expired = false;
          continue;
        }
      }
      e.client->MarkDirty();
    }
  }
}

std::vector<std::string> WatchRegistry::ExistingWatched(uint8_t db, ReadView& view) {
  std::vector<std::string> out;
  for (Shard& s : shards_) {
    std::lock_guard lock(s.mu);
    for (const auto& [mk, entries] : s.by_key) {
      if (static_cast<uint8_t>(mk[0]) != db) continue;
      const std::string_view key = std::string_view(mk).substr(1);
      if (MetaExists(view, db, key)) out.emplace_back(key);
    }
  }
  return out;
}

}  // namespace tideskv
