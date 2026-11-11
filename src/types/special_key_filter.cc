#include "types/special_key_filter.h"

#include <algorithm>
#include <functional>
#include <utility>

#include "encoding/meta_codec.h"

namespace tideskv {
namespace {

uint64_t Splitmix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

struct Probe {
  size_t block;
  uint64_t h2;  // 7 probes of 9 bits each
};

Probe Locate(std::string_view key, size_t blocks) {
  const uint64_t h1 = std::hash<std::string_view>{}(key);
  const uint64_t high = h1 >> 32;
  return {static_cast<size_t>((high * blocks) >> 32), Splitmix64(h1)};
}

}  // namespace

SpecialKeyFilter::SpecialKeyFilter(size_t initial_capacity, size_t max_bytes)
    : max_capacity_(std::max<size_t>(1, max_bytes * 8 / kBitsPerKey)) {
  Allocate(initial_capacity);
}

// Strong guarantee: if the allocation throws, the old bits, block count, capacity and count stay as they
// were.
void SpecialKeyFilter::Allocate(size_t capacity) {
  capacity = std::max<size_t>(1, std::min(capacity, max_capacity_));
  const size_t blocks = std::max<size_t>(1, (capacity * kBitsPerKey + 511) / 512);
  std::unique_ptr<std::atomic<uint64_t>[]> words(new std::atomic<uint64_t>[blocks * kWordsPerBlock]());
  words_ = std::move(words);
  blocks_ = blocks;
  capacity_.store(capacity, std::memory_order_relaxed);
  count_.store(0, std::memory_order_relaxed);
}

void SpecialKeyFilter::OnMetaPut(std::string_view meta_key, std::string_view value) noexcept {
  if (encoding::MetaIsSpecial(value)) Insert(meta_key);
}

void SpecialKeyFilter::Insert(std::string_view meta_key) noexcept {
  const Probe p = Locate(meta_key, blocks_);
  std::atomic<uint64_t>* block = &words_[p.block * kWordsPerBlock];
  bool added = false;
  for (int i = 0; i < kProbes; ++i) {
    const uint32_t bit = static_cast<uint32_t>(p.h2 >> (9 * i)) & 511u;
    const uint64_t mask = uint64_t{1} << (bit & 63);
    std::atomic<uint64_t>& w = block[bit >> 6];
    if (w.load(std::memory_order_relaxed) & mask) continue;
    if (!(w.fetch_or(mask, std::memory_order_relaxed) & mask)) added = true;
  }
  if (added) count_.fetch_add(1, std::memory_order_relaxed);
}

bool SpecialKeyFilter::MayContain(std::string_view meta_key) const noexcept {
  const Probe p = Locate(meta_key, blocks_);
  const std::atomic<uint64_t>* block = &words_[p.block * kWordsPerBlock];
  for (int i = 0; i < kProbes; ++i) {
    const uint32_t bit = static_cast<uint32_t>(p.h2 >> (9 * i)) & 511u;
    if (!(block[bit >> 6].load(std::memory_order_relaxed) & (uint64_t{1} << (bit & 63)))) return false;
  }
  return true;
}

bool SpecialKeyFilter::SkipReadAllowed(std::string_view meta_key) const noexcept {
  return enabled() && state() == State::kReady && !MayContain(meta_key);
}

void SpecialKeyFilter::set_enabled(bool on) noexcept { enabled_.store(on, std::memory_order_relaxed); }
bool SpecialKeyFilter::enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }

void SpecialKeyFilter::Reset(size_t capacity) {
  Allocate(capacity);  // may throw std::bad_alloc: the old filter and its state are then left intact
  state_.store(State::kBuilding, std::memory_order_release);
}

void SpecialKeyFilter::MarkReady() noexcept {
  const bool saturated = Count() > Capacity() && Capacity() == MaxCapacity();
  state_.store(saturated ? State::kSaturated : State::kReady, std::memory_order_release);
}

bool SpecialKeyFilter::NeedsGrowth() const noexcept {
  return Count() > Capacity() && Capacity() < MaxCapacity();
}

SpecialKeyFilter::State SpecialKeyFilter::state() const noexcept {
  return state_.load(std::memory_order_acquire);
}

std::string_view SpecialKeyFilter::state_name() const noexcept {
  if (!enabled()) return "disabled";
  switch (state()) {
    case State::kBuilding:
      return "building";
    case State::kReady:
      return "ready";
    case State::kSaturated:
      return "saturated";
  }
  return "building";
}

size_t SpecialKeyFilter::Count() const noexcept { return count_.load(std::memory_order_relaxed); }
size_t SpecialKeyFilter::Capacity() const noexcept { return capacity_.load(std::memory_order_relaxed); }
size_t SpecialKeyFilter::MaxCapacity() const noexcept { return max_capacity_; }
size_t SpecialKeyFilter::Bytes() const noexcept { return blocks_ * kWordsPerBlock * sizeof(uint64_t); }
void SpecialKeyFilter::NoteSkippedRead() noexcept { skipped_.fetch_add(1, std::memory_order_relaxed); }
uint64_t SpecialKeyFilter::skipped_reads() const noexcept { return skipped_.load(std::memory_order_relaxed); }

Result<SpecialKeyCheck> CheckSpecialKeys(ReadView& view, const SpecialKeyFilter& filter) {
  auto it = view.NewIterator(ColumnFamily::kMeta);
  if (!it.ok()) return it.status();
  SpecialKeyCheck out;
  Status st = (*it)->SeekToFirst();
  while (st.ok() && (*it)->Valid()) {
    if (encoding::MetaIsSpecial((*it)->value())) {
      ++out.specials;
      if (!filter.MayContain((*it)->key())) ++out.missing;
    }
    st = (*it)->Next();
  }
  if (!st.ok()) return st;
  return out;
}

}  // namespace tideskv
