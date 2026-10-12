#include "exec/lock_manager.h"

#include <algorithm>

namespace tideskv {

size_t LockManager::StripeOf(uint8_t db, std::string_view key) {
  uint64_t h = 1469598103934665603ULL;  // FNV-1a
  auto mix = [&h](unsigned char c) {
    h ^= c;
    h *= 1099511628211ULL;
  };
  mix(db);
  for (char c : key) mix(static_cast<unsigned char>(c));
  return static_cast<size_t>(h % kStripes);
}

LockManager::Guard LockManager::Lock(uint8_t db, const std::vector<std::string_view>& keys) {
  std::vector<uint16_t> stripes;
  stripes.reserve(keys.size());
  for (std::string_view k : keys) stripes.push_back(static_cast<uint16_t>(StripeOf(db, k)));
  std::sort(stripes.begin(), stripes.end());
  stripes.erase(std::unique(stripes.begin(), stripes.end()), stripes.end());
  for (uint16_t s : stripes) mutexes_[s].lock();
  return Guard(this, std::move(stripes));
}

LockManager::Guard::~Guard() {
  if (owner_ == nullptr) return;
  for (auto it = stripes_.rbegin(); it != stripes_.rend(); ++it) owner_->mutexes_[*it].unlock();
}

}  // namespace tideskv
