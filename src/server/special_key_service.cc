#include "server/special_key_service.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <new>
#include <string>
#include <utility>

#include "encoding/meta_codec.h"

namespace tideskv {
namespace {

constexpr auto kPollInterval = std::chrono::milliseconds(100);
constexpr size_t kGrowthFactor = 4;
constexpr uint64_t kScanHeadroom = 2;  // a rebuild after a scan that overflowed is sized for twice its count
constexpr size_t kStopCheckEvery = 1024;  // keys between two looks at the stop flag

// One chunk from a fresh view: up to kScanChunk keys after *last (from the first key when !*started).
// *done once the CF is exhausted.
Status ScanChunk(Storage& storage, SpecialKeyFilter& filter, const std::atomic<bool>& stop, std::string* last,
                 bool* started, bool* done, uint64_t* specials) {
  auto view = storage.BeginRead();
  if (!view.ok()) return view.status();
  // Not through the block cache: this thread is long-lived and mostly idle, so blocks it cached would be
  // freed cross-thread by the workers that evict them, which neither allocator lets them reuse (about one
  // extra cache of resident memory), and a full pass would replace the workload's hot set anyway.
  IterOptions opts;
  opts.fill_cache = false;
  auto it = (*view)->NewIterator(ColumnFamily::kMeta, std::move(opts));
  if (!it.ok()) return it.status();
  Iterator& i = **it;
  Status st = *started ? i.Seek(*last) : i.SeekToFirst();
  if (st.ok() && *started && i.Valid() && i.key() == *last) st = i.Next();  // strictly after it
  size_t n = 0;
  while (st.ok() && i.Valid() && n < kScanChunk) {
    if (n % kStopCheckEvery == 0 && stop.load(std::memory_order_relaxed)) return Status::Ok();
    if (encoding::MetaIsSpecial(i.value())) {
      filter.Insert(i.key());
      ++*specials;
    }
    last->assign(i.key());
    *started = true;
    ++n;
    st = i.Next();
  }
  if (!st.ok()) return st;
  *done = !i.Valid();
  return Status::Ok();
}

}  // namespace

Result<uint64_t> ScanSpecialKeys(Storage& storage, SpecialKeyFilter& filter, const std::atomic<bool>& stop) {
  std::string last;
  bool started = false;
  bool done = false;
  uint64_t specials = 0;
  while (!done && !stop.load(std::memory_order_relaxed)) {
    if (Status st = ScanChunk(storage, filter, stop, &last, &started, &done, &specials); !st.ok()) return st;
  }
  return specials;
}

size_t GrownFilterCapacity(size_t capacity, uint64_t specials, size_t max_capacity) {
  const uint64_t wanted = std::max<uint64_t>(uint64_t{capacity} * kGrowthFactor, specials * kScanHeadroom);
  return static_cast<size_t>(std::min<uint64_t>(wanted, max_capacity));
}

std::chrono::milliseconds RebuildRetryDelay(unsigned failures, std::chrono::milliseconds base,
                                            std::chrono::milliseconds max) {
  if (failures == 0 || base >= max) return std::min(base, max);
  std::chrono::milliseconds d = base;
  for (unsigned i = 1; i < failures && d < max; ++i) d *= 2;  // stops doubling at the cap: no overflow
  return std::min(d, max);
}

SpecialKeyService::SpecialKeyService(SpecialKeyFilter& filter, Storage& storage, Engine& engine,
                                     SpecialKeyRetryPolicy retry)
    : filter_(filter), storage_(storage), engine_(engine), retry_(retry) {}

SpecialKeyService::~SpecialKeyService() { Stop(); }

void SpecialKeyService::Start() {
  if (thread_.joinable()) return;
  stop_.store(false);
  thread_ = std::thread([this] { Run(); });
}

void SpecialKeyService::Stop() {
  {
    std::lock_guard lock(mu_);
    stop_.store(true);
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

uint64_t SpecialKeyService::rebuilds() const { return rebuilds_.load(std::memory_order_relaxed); }

Status SpecialKeyService::RebuildNow(size_t capacity) {
  // Allocated and zeroed before the gate (up to 64 MiB of page faults), so reads and writes wait only for
  // the O(1) swap. On bad_alloc the gate is never taken and the old filter stays as it was, complete.
  SpecialKeyFilter::Bits next;
  try {
    next = filter_.PrepareBits(capacity);
  } catch (const std::bad_alloc&) {
    return Status::Internal("cannot allocate the special-key filter for " + std::to_string(capacity) +
                            " keys");
  }
  SpecialKeyFilter::Bits old;
  // Under the barrier: no meta Put is buffered and uncommitted now.
  Status quiesced = engine_.Quiesce([&] { old = filter_.Install(std::move(next)); });
  old = SpecialKeyFilter::Bits();  // the previous bits are freed here, outside the gate
  if (!quiesced.ok()) return quiesced;
  auto scanned = ScanSpecialKeys(storage_, filter_, stop_);
  if (!scanned.ok()) return scanned.status();                             // stays kBuilding
  if (stop_.load()) return Status::Internal("special-key scan stopped");  // partial: never ready
  last_scan_specials_ = *scanned;
  filter_.MarkReady();
  rebuilds_.fetch_add(1, std::memory_order_relaxed);
  return Status::Ok();
}

bool SpecialKeyService::WaitFor(std::chrono::milliseconds d) {
  std::unique_lock lock(mu_);
  cv_.wait_for(lock, d, [this] { return stop_.load(); });
  return !stop_.load();
}

// A filter at its size limit that holds more keys than it was built for: SETs read again (a blind SET
// needs kReady), so say so once.
void SpecialKeyService::NoteSaturation() {
  if (filter_.state() == SpecialKeyFilter::State::kReady && filter_.Count() > filter_.Capacity() &&
      filter_.Capacity() == filter_.MaxCapacity()) {
    filter_.MarkReady();  // -> kSaturated
  }
  if (filter_.state() != SpecialKeyFilter::State::kSaturated) {
    if (filter_.state() == SpecialKeyFilter::State::kReady) saturation_warned_ = false;
    return;
  }
  if (saturation_warned_) return;
  saturation_warned_ = true;
  spdlog::warn(
      "special-key filter saturated at {} keys ({} bytes): every SET reads before writing until a restart or "
      "a full resync",
      filter_.Capacity(), filter_.Bytes());
}

void SpecialKeyService::Run() {
  bool rebuild = true;  // the startup build
  size_t capacity = filter_.Capacity();
  unsigned failures = 0;  // consecutive failed rebuilds: the retry backs off so a persistent failure does
                          // not take the exclusive gate (and rescan) every second
  bool saw_loading = false;
  while (!stop_.load()) {
    if (rebuild) {
      const Status st = RebuildNow(capacity);
      if (stop_.load()) return;
      if (!st.ok()) {
        ++failures;
        const auto delay = RebuildRetryDelay(failures, retry_.base, retry_.max);
        if (failures == 1)
          spdlog::warn("special-key filter rebuild failed (retrying with backoff from {} ms to {} ms): {}",
                       retry_.base.count(), retry_.max.count(), st.ToString());
        if (!WaitFor(delay)) return;
        continue;
      }
      if (failures > 0) spdlog::info("special-key filter rebuilt after {} failed attempts", failures);
      failures = 0;
      rebuild = false;
      NoteSaturation();
    }
    if (!WaitFor(kPollInterval)) return;
    if (engine_.loading()) {  // a full-sync load replaces the dataset: rebuild once it ends
      saw_loading = true;
      continue;
    }
    const bool grow = filter_.NeedsGrowth();
    if (saw_loading || grow) {
      saw_loading = false;
      rebuild = true;
      // Sized from what the last scan counted (Count() undercounts an overfull filter), so a restart over
      // many specials rescans once or twice instead of growing 4x per pass.
      capacity = grow ? GrownFilterCapacity(filter_.Capacity(),
                                            std::max<uint64_t>(last_scan_specials_, filter_.Count()),
                                            filter_.MaxCapacity())
                      : filter_.Capacity();
      continue;
    }
    NoteSaturation();
  }
}

}  // namespace tideskv
