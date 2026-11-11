#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "common/status.h"
#include "storage/storage.h"

namespace tideskv {

// Keys (as encoded meta-CF keys) whose meta record is, or once was, a collection or carried a TTL.
// A blocked bloom: 512-bit blocks, kBitsPerKey bits per key, 7 probes in one block, words are
// std::atomic<uint64_t> set with relaxed fetch_or. No deletes: a key leaves only through Reset.
// Hash: std::hash<std::string_view>, then a splitmix64 finalizer for the second half.
// OnMetaPut/Insert/MayContain/SkipReadAllowed may run concurrently. Reset must not run concurrently
// with any of them (it swaps the bit array without synchronisation): SpecialKeyService runs it under
// Engine::Quiesce, and every meta writer and every SET holds the Engine gate (the gate's mutex also
// orders a writer's insert before a later SET's check). The object itself is long-lived: open
// transactions cache the raw MetaPutHook pointer.
// A false negative would let a plain SET skip its pre-write read over live subkeys/TTL; a false
// positive only costs speed.
class SpecialKeyFilter final : public MetaPutHook {
 public:
  static constexpr size_t kInitialCapacity = size_t{1} << 20;  // keys
  static constexpr size_t kMaxBytes = size_t{64} << 20;
  static constexpr int kBitsPerKey = 10;
  enum class State : uint8_t { kBuilding, kReady, kSaturated };

  explicit SpecialKeyFilter(size_t initial_capacity = kInitialCapacity, size_t max_bytes = kMaxBytes);

  void OnMetaPut(std::string_view meta_key, std::string_view value) noexcept override;  // Insert if special
  void Insert(std::string_view meta_key) noexcept;
  bool MayContain(std::string_view meta_key) const noexcept;
  // enabled && state() == kReady && !MayContain(meta_key)
  bool SkipReadAllowed(std::string_view meta_key) const noexcept;

  void set_enabled(bool on) noexcept;
  bool enabled() const noexcept;

  // Not concurrent with any other call (see above).
  void Reset(size_t capacity);        // empty bits for min(capacity, MaxCapacity()) keys; state kBuilding
  void MarkReady() noexcept;          // kReady, or kSaturated when Count() > Capacity() == MaxCapacity()
  bool NeedsGrowth() const noexcept;  // Count() > Capacity() && Capacity() < MaxCapacity()

  State state() const noexcept;
  std::string_view state_name() const noexcept;  // "building" | "ready" | "saturated" | "disabled"
  size_t Count() const noexcept;                 // inserts that set at least one new bit since the last Reset
  size_t Capacity() const noexcept;
  size_t MaxCapacity() const noexcept;  // max_bytes * 8 / kBitsPerKey
  size_t Bytes() const noexcept;
  void NoteSkippedRead() noexcept;  // relaxed counter, one per blind SET key
  uint64_t skipped_reads() const noexcept;

 private:
  static constexpr size_t kWordsPerBlock = 8;  // 512 bits
  static constexpr int kProbes = 7;

  void Allocate(size_t capacity);

  const size_t max_capacity_;
  std::unique_ptr<std::atomic<uint64_t>[]> words_;
  size_t blocks_ = 0;
  std::atomic<size_t> capacity_{0};
  std::atomic<size_t> count_{0};
  std::atomic<State> state_{State::kBuilding};
  std::atomic<bool> enabled_{true};
  std::atomic<uint64_t> skipped_{0};
};

struct SpecialKeyCheck {
  uint64_t specials = 0;  // meta records for which MetaIsSpecial holds
  uint64_t missing = 0;   // of those, keys the filter does not contain
};

// Scans the whole meta CF of `view` (O(keys); for DEBUG and tests).
Result<SpecialKeyCheck> CheckSpecialKeys(ReadView& view, const SpecialKeyFilter& filter);

}  // namespace tideskv
