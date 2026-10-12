#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

#include "types/scan_position.h"

namespace tideskv {

// Maps numeric SCAN cursors to scan positions (M2 plan D9): clients parse cursors as
// integers, so the encoded meta key can't be the cursor itself. Thread-safe; shared by all
// connections, like Redis cursors. Bounded by entry count and by bytes: the oldest entries
// are evicted to make room, so a client can't exhaust memory by saving positions next to
// huge keys. An evicted or unknown cursor restarts the scan (allowed by D9).
class CursorStore {
 public:
  static constexpr size_t kCapacity = 65536;
  static constexpr size_t kMaxBytes = size_t{64} << 20;         // all saved positions together
  static constexpr size_t kMaxPositionBytes = size_t{1} << 20;  // one saved position

  // A position longer than kMaxPositionBytes is saved truncated: its prefix of that length
  // plus the full position's size and PositionDigest. On resume ScanKeys steps past the key
  // sharing the prefix with that size and digest; if it is gone, the scan resumes at the
  // prefix, which sorts at or before the full position, so it can repeat keys (allowed by
  // SCAN) but never skip one. A digest collision among same-size keys sharing the prefix
  // could make a scan resume too far or loop forever; PositionDigest is keyed per process,
  // so producing one requires breaking a keyed PRF (SipHash-2-4).
  uint64_t Save(std::string position);
  std::optional<ScanPosition> Load(uint64_t cursor) const;
  size_t bytes() const;  // accounted size of the saved entries; never above kMaxBytes

 private:
  struct Entry {
    uint64_t cursor = 0;
    ScanPosition position;
  };
  static size_t Cost(const Entry& e) { return sizeof(Entry) + e.position.key.capacity(); }

  mutable std::mutex mu_;
  std::deque<Entry> entries_;  // consecutive cursors, oldest first
  size_t bytes_ = 0;
  uint64_t last_ = 0;
};

}  // namespace tideskv
