#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tideskv {

// Where a SCAN resumes: strictly after the meta key `key`. A meta key longer than
// CursorStore::kMaxPositionBytes is kept as its prefix of that length plus the full key's
// size and digest; ScanKeys walks the keys sharing the prefix and resumes after the one
// with that size and digest (see scan.h).
struct ScanPosition {
  std::string key;       // the full meta key, or its prefix when truncated; empty = start
  size_t full_size = 0;  // size of the full meta key
  uint64_t digest = 0;   // PositionDigest(full meta key); set only when truncated
  bool truncated = false;
};

// SipHash-2-4 of `s` under a 128-bit key drawn once per process from std::random_device.
// Cursors live only in memory, so digests need not agree across processes. Keying matters:
// two same-size keys sharing the saved prefix with equal digests can make a SCAN loop (the
// resume walk matches the earlier key, re-examines up to the later one and saves it again),
// so crafting such a pair must require breaking a keyed PRF.
uint64_t PositionDigest(std::string_view s);

using SipHashKey = std::array<uint8_t, 16>;
// SipHash-2-4 with an explicit key (k0 = bytes 0..7, k1 = bytes 8..15, little-endian).
uint64_t SipHash24(const SipHashKey& key, std::string_view s);

}  // namespace tideskv
