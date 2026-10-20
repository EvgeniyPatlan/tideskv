#pragma once
#include <atomic>
#include <cstdint>
#include <optional>
#include <string_view>

// Whether keyspace keys carry the CRC16 slot after the db byte (slotless key layout design, 2026-10-20).
//   slotted  (format_version "1"): meta [db][slot][key], expire [db][ms][slot][key], objects [db][slot][oid]
//   slotless (format_version "2"): the same keys without the slot. A cluster node is always slotted.
// One process-wide value, chosen once at boot from the data dir (OpenFormat) before the Engine exists; a
// replica's full sync may switch it under the Engine's load barrier (AdoptKeyLayout). Every codec function
// reads it: one relaxed load of a value that changes at most at those two points.
namespace tideskv::encoding {

enum class KeyLayout : uint8_t { kSlotted, kSlotless };

namespace key_layout_internal {
inline std::atomic<KeyLayout> g_layout{KeyLayout::kSlotted};  // the default keeps the pre-layout bytes
}  // namespace key_layout_internal

inline KeyLayout CurrentKeyLayout() { return key_layout_internal::g_layout.load(std::memory_order_relaxed); }
inline bool Slotless() { return CurrentKeyLayout() == KeyLayout::kSlotless; }

// The boot choice. False (and no change) on a second call: the layout is set once per process.
bool InitKeyLayout(KeyLayout layout);
// A replica adopting its master's layout on a full sync. Only under the Engine's load barrier, with no
// command running and every keyspace CF about to be wiped.
void AdoptKeyLayout(KeyLayout layout);

// The slot a key's encodings carry: KeySlot(key), or 0 when slotless (no CRC16 computed).
uint16_t LayoutSlot(std::string_view user_key);

std::string_view KeyLayoutName(KeyLayout layout);  // "slotted" / "slotless" (REPLCONF key-layout)
std::optional<KeyLayout> ParseKeyLayout(std::string_view name);

// Tests: sets the layout for a scope and restores the previous one. Not for production code.
class ScopedKeyLayout {
 public:
  explicit ScopedKeyLayout(KeyLayout layout) : previous_(CurrentKeyLayout()) {
    key_layout_internal::g_layout.store(layout, std::memory_order_relaxed);
  }
  ~ScopedKeyLayout() { key_layout_internal::g_layout.store(previous_, std::memory_order_relaxed); }
  ScopedKeyLayout(const ScopedKeyLayout&) = delete;
  ScopedKeyLayout& operator=(const ScopedKeyLayout&) = delete;

 private:
  KeyLayout previous_;
};

}  // namespace tideskv::encoding
