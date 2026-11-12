#include "server/special_key_service.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <new>
#include <string>
#include <utility>

#include "encoding/meta_codec.h"

namespace tideskv {
namespace {

constexpr auto kPollInterval = std::chrono::milliseconds(100);
constexpr auto kRetryInterval = std::chrono::seconds(1);
constexpr size_t kGrowthFactor = 4;
constexpr size_t kStopCheckEvery = 1024;  // keys between two looks at the stop flag

// One chunk from a fresh view: up to kScanChunk keys after *last (from the first key when !*started).
// *done once the CF is exhausted.
Status ScanChunk(Storage& storage, SpecialKeyFilter& filter, const std::atomic<bool>& stop, std::string* last,
                 bool* started, bool* done) {
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
    if (encoding::MetaIsSpecial(i.value())) filter.Insert(i.key());
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

Status ScanSpecialKeys(Storage& storage, SpecialKeyFilter& filter, const std::atomic<bool>& stop) {
  std::string last;
  bool started = false;
  bool done = false;
  while (!done && !stop.load(std::memory_order_relaxed)) {
    if (Status st = ScanChunk(storage, filter, stop, &last, &started, &done); !st.ok()) return st;
  }
  return Status::Ok();
}

SpecialKeyService::SpecialKeyService(SpecialKeyFilter& filter, Storage& storage, Engine& engine)
    : filter_(filter), storage_(storage), engine_(engine) {}

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
  Status reset = Status::Ok();
  const Status quiesced = engine_.Quiesce([&] {
    try {
      filter_.Reset(capacity);  // under the barrier: no meta Put is buffered and uncommitted now
    } catch (const std::bad_alloc&) {
      reset = Status::Internal("cannot allocate the special-key filter for " + std::to_string(capacity) +
                               " keys");  // the old filter stays as it was, complete
    }
  });
  if (!quiesced.ok()) return quiesced;
  if (!reset.ok()) return reset;
  if (Status st = ScanSpecialKeys(storage_, filter_, stop_); !st.ok()) return st;  // stays kBuilding
  if (stop_.load()) return Status::Internal("special-key scan stopped");           // partial: never ready
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
  bool failing = false;
  bool saw_loading = false;
  while (!stop_.load()) {
    if (rebuild) {
      const Status st = RebuildNow(capacity);
      if (stop_.load()) return;
      if (!st.ok()) {
        if (!failing)
          spdlog::warn("special-key filter rebuild failed (retrying every second): {}", st.ToString());
        failing = true;
        if (!WaitFor(kRetryInterval)) return;
        continue;
      }
      if (failing) spdlog::info("special-key filter rebuilt after an earlier failure");
      failing = false;
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
      capacity = grow ? filter_.Capacity() * kGrowthFactor : filter_.Capacity();
      continue;
    }
    NoteSaturation();
  }
}

}  // namespace tideskv
