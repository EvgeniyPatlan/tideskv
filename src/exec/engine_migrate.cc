// The migration entries of the Engine (M7b D4): the slot write fence. Task 4 adds the internal writers.
#include <cassert>
#include <cstdint>
#include <functional>
#include <mutex>

#include "exec/engine.h"
#include "exec/engine_internal.h"

namespace tideskv {

uint64_t Engine::ChangeFence(const std::function<void(SlotBits&)>& edit) {
  assert(ActiveBatch() == nullptr && "a fence change inside a batch deadlocks on the gate");
  [[maybe_unused]] const engine_internal::ReentryCheck reentry;
  std::lock_guard barrier(gate_);  // every write holding the gate finishes first (D4), busy or not
  SlotBits bits = fence_.Bits();
  edit(bits);
  fence_.Assign(bits);
  return repl_log_ != nullptr ? repl_log_->last() : 0;  // inside the barrier: the end of the final delta
}

uint64_t Engine::FenceSlots(uint16_t first, uint16_t last) {
  return ChangeFence([&](SlotBits& b) {
    for (uint32_t s = first; s <= last && s < kSlotCount; ++s) b.set(s);
  });
}

uint64_t Engine::UnfenceSlots(uint16_t first, uint16_t last) {
  return ChangeFence([&](SlotBits& b) {
    for (uint32_t s = first; s <= last && s < kSlotCount; ++s) b.reset(s);
  });
}

uint64_t Engine::AssignFence(const SlotBits& bits) {
  return ChangeFence([&](SlotBits& b) { b = bits; });
}

}  // namespace tideskv
