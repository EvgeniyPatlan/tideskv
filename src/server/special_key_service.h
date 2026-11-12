#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

#include "common/status.h"
#include "exec/engine.h"
#include "storage/storage.h"
#include "types/special_key_filter.h"

namespace tideskv {

// Fills a SpecialKeyFilter from the meta CF: chunks of kScanChunk keys, each through a fresh BeginRead
// view and iterator, resuming strictly after the last key seen; Insert for every MetaIsSpecial value.
// Returns the number of special records it found (so far, when it returns early - OK - because stop is set).
// Reads without filling the block cache (IterOptions::fill_cache = false).
inline constexpr size_t kScanChunk = 65536;
Result<uint64_t> ScanSpecialKeys(Storage& storage, SpecialKeyFilter& filter, const std::atomic<bool>& stop);

// The capacity a growth rebuild uses: 4x the current one, or twice the specials the last scan (or the hook
// since) counted when that is more, at most max_capacity. A restart over many specials thus rescans once
// after the first scan overflows the filter, instead of once per 4x step.
size_t GrownFilterCapacity(size_t capacity, uint64_t specials, size_t max_capacity);

// The wait before retry number `failures` (1 = after the first failure): base doubling per failure, at
// most max.
std::chrono::milliseconds RebuildRetryDelay(unsigned failures, std::chrono::milliseconds base,
                                            std::chrono::milliseconds max);

// B1: builds the filter at start and rebuilds it when it must grow or a full-sync load replaced the
// dataset. Why a rebuild misses no key: Install runs under Engine::Quiesce, which waits for every write
// holding the gate (and every meta Put happens under it), so each meta Put either committed before the
// Install (and the scan, whose views all open after it, reads it) or is made after it (and the hook inserts
// it into the new bits). The filter is kReady only after a complete scan.
// The filter must outlive the service and every write transaction (they cache the hook pointer).
// A failed rebuild is retried after RebuildRetryDelay(consecutive failures, base, max); success resets it.
struct SpecialKeyRetryPolicy {
  std::chrono::milliseconds base{std::chrono::seconds(1)};
  std::chrono::milliseconds max{std::chrono::seconds(60)};
};

class SpecialKeyService {
 public:
  SpecialKeyService(SpecialKeyFilter& filter, Storage& storage, Engine& engine,
                    SpecialKeyRetryPolicy retry = {});
  ~SpecialKeyService();  // Stop()
  SpecialKeyService(const SpecialKeyService&) = delete;
  SpecialKeyService& operator=(const SpecialKeyService&) = delete;

  void Start();  // background thread: RebuildNow(filter.Capacity()), then the watch loop
  void Stop();   // sets stop, joins; idempotent
  // filter.PrepareBits(capacity) outside the gate, engine.Quiesce([&]{ filter.Install(...); }) (the old
  // bits are freed after it), ScanSpecialKeys, filter.MarkReady(). On an allocation failure nothing changes
  // and the gate is not taken. On a scan error the filter stays kBuilding (SET reads) and the error is
  // returned. Never concurrent with itself: the service thread is its only caller once Start ran.
  Status RebuildNow(size_t capacity);
  uint64_t rebuilds() const;  // completed RebuildNow calls

 private:
  void Run();
  bool WaitFor(std::chrono::milliseconds d);  // false once stop is set
  void NoteSaturation();

  SpecialKeyFilter& filter_;
  Storage& storage_;
  Engine& engine_;
  const SpecialKeyRetryPolicy retry_;
  uint64_t last_scan_specials_ = 0;  // specials the last complete scan found; RebuildNow's caller only
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> rebuilds_{0};
  std::mutex mu_;
  std::condition_variable cv_;
  std::thread thread_;
  bool saturation_warned_ = false;  // service thread only
};

}  // namespace tideskv
