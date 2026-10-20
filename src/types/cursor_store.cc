#include "types/cursor_store.h"

#include <utility>

namespace tideskv {

namespace {

ScanPosition MakePosition(std::string full, size_t max_bytes) {
  ScanPosition pos;
  pos.full_size = full.size();
  if (full.size() > max_bytes) {
    pos.digest = PositionDigest(full);
    pos.truncated = true;
    full.resize(max_bytes);
    full.shrink_to_fit();
  }
  pos.key = std::move(full);
  return pos;
}

}  // namespace

uint64_t CursorStore::Save(std::string position, CursorKind kind) {
  ScanPosition pos = MakePosition(std::move(position), kMaxPositionBytes);
  std::lock_guard lock(mu_);
  Entry entry{++last_, kind, std::move(pos)};
  const size_t cost = Cost(entry);
  while (!entries_.empty() && (entries_.size() >= kCapacity || bytes_ + cost > kMaxBytes)) {
    bytes_ -= Cost(entries_.front());
    entries_.pop_front();
  }
  bytes_ += cost;
  entries_.push_back(std::move(entry));
  return last_;
}

std::optional<ScanPosition> CursorStore::Load(uint64_t cursor, CursorKind kind) const {
  if (cursor == 0) return std::nullopt;
  std::lock_guard lock(mu_);
  if (entries_.empty() || cursor < entries_.front().cursor || cursor > entries_.back().cursor) {
    return std::nullopt;
  }
  const Entry& entry = entries_[cursor - entries_.front().cursor];
  if (entry.kind != kind) return std::nullopt;
  return entry.position;
}

size_t CursorStore::bytes() const {
  std::lock_guard lock(mu_);
  return bytes_;
}

std::string CursorStore::RandomKeyCursor(uint8_t db) const {
  std::lock_guard lock(mu_);
  return db < random_key_cursors_.size() ? random_key_cursors_[db] : std::string();
}

void CursorStore::SetRandomKeyCursor(uint8_t db, std::string key) {
  if (key.size() > kMaxPositionBytes) key.clear();
  std::lock_guard lock(mu_);
  if (db < random_key_cursors_.size()) random_key_cursors_[db] = std::move(key);
}

}  // namespace tideskv
