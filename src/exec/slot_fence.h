#pragma once
#include <array>
#include <atomic>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "common/crc16.h"

// The per-slot write fence of a migrating source (M7b D4). Readers call Test/AnyKey on any thread without a
// lock: one that holds the Engine gate is ordered by the gate, one that does not (the reaper's filter) only
// needs a value that held at some instant. Assign is called only with the Engine gate held exclusively: that
// barrier, not these atomics, makes the fence exact.
namespace tideskv {

using SlotBits = std::bitset<kSlotCount>;

class SlotFence {
 public:
  bool any() const { return any_.load(std::memory_order_acquire); }
  bool Test(uint16_t slot) const {
    return ((words_[slot / 64].load(std::memory_order_relaxed) >> (slot % 64)) & 1u) != 0;
  }
  // True when one of `keys` hashes to a fenced slot.
  bool AnyKey(const std::vector<std::string_view>& keys) const {
    if (!any()) return false;
    for (std::string_view k : keys) {
      if (Test(KeySlot(k))) return true;
    }
    return false;
  }
  // A diagnostic snapshot: without the Engine gate it may mix two Assigns; exact only under the gate.
  SlotBits Bits() const {
    SlotBits out;
    for (size_t s = 0; s < kSlotCount; ++s) out[s] = Test(static_cast<uint16_t>(s));
    return out;
  }
  void Assign(const SlotBits& bits) {
    for (size_t w = 0; w < kWords; ++w) {
      uint64_t word = 0;
      for (size_t b = 0; b < 64; ++b) word |= static_cast<uint64_t>(bits[w * 64 + b]) << b;
      words_[w].store(word, std::memory_order_relaxed);
    }
    any_.store(bits.any(), std::memory_order_release);
  }

 private:
  static constexpr size_t kWords = kSlotCount / 64;
  std::array<std::atomic<uint64_t>, kWords> words_{};
  std::atomic<bool> any_{false};
};

}  // namespace tideskv
