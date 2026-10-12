#include "types/cursor_store.h"

#include <utility>

namespace tideskv {

uint64_t CursorStore::Save(std::string position) {
  if (position.size() > kMaxPositionBytes) {
    position.resize(kMaxPositionBytes);
    position.shrink_to_fit();
  }
  std::lock_guard lock(mu_);
  Entry entry{++last_, std::move(position)};
  const size_t cost = Cost(entry);
  while (!entries_.empty() && (entries_.size() >= kCapacity || bytes_ + cost > kMaxBytes)) {
    bytes_ -= Cost(entries_.front());
    entries_.pop_front();
  }
  bytes_ += cost;
  entries_.push_back(std::move(entry));
  return last_;
}

std::optional<std::string> CursorStore::Load(uint64_t cursor) const {
  if (cursor == 0) return std::nullopt;
  std::lock_guard lock(mu_);
  if (entries_.empty() || cursor < entries_.front().cursor || cursor > entries_.back().cursor) {
    return std::nullopt;
  }
  return entries_[cursor - entries_.front().cursor].position;
}

size_t CursorStore::bytes() const {
  std::lock_guard lock(mu_);
  return bytes_;
}

}  // namespace tideskv
