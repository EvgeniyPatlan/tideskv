#include "encoding/key_layout.h"

#include "common/crc16.h"

namespace tideskv::encoding {
namespace {

std::atomic<bool> g_initialised{false};

}  // namespace

bool InitKeyLayout(KeyLayout layout) {
  if (g_initialised.exchange(true, std::memory_order_acq_rel)) return false;
  key_layout_internal::g_layout.store(layout, std::memory_order_relaxed);
  return true;
}

void AdoptKeyLayout(KeyLayout layout) {
  key_layout_internal::g_layout.store(layout, std::memory_order_relaxed);
}

uint16_t LayoutSlot(std::string_view user_key) { return Slotless() ? 0 : KeySlot(user_key); }

std::string_view KeyLayoutName(KeyLayout layout) {
  return layout == KeyLayout::kSlotless ? "slotless" : "slotted";
}

std::optional<KeyLayout> ParseKeyLayout(std::string_view name) {
  if (name == "slotted") return KeyLayout::kSlotted;
  if (name == "slotless") return KeyLayout::kSlotless;
  return std::nullopt;
}

}  // namespace tideskv::encoding
