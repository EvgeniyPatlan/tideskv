#include "types/cursor_store.h"

#include <utility>

namespace tideskv {

uint64_t CursorStore::Save(std::string position) {
  std::lock_guard lock(mu_);
  const uint64_t cursor = ++last_;
  entries_[cursor % kCapacity] = {cursor, std::move(position)};
  return cursor;
}

std::optional<std::string> CursorStore::Load(uint64_t cursor) const {
  if (cursor == 0) return std::nullopt;
  std::lock_guard lock(mu_);
  const Entry& e = entries_[cursor % kCapacity];
  if (e.cursor != cursor) return std::nullopt;
  return e.position;
}

}  // namespace tideskv
