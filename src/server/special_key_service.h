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
// Returns early (OK) when stop is set.
inline constexpr size_t kScanChunk = 65536;
Status ScanSpecialKeys(Storage& storage, SpecialKeyFilter& filter, const std::atomic<bool>& stop);

// B1: builds the filter at start and rebuilds it when it must grow or a full-sync load replaced the
// dataset. Why a rebuild misses no key: Reset runs under Engine::Quiesce, which waits for every write
// holding the gate (and every meta Put happens under it), so each meta Put either committed before the
// Reset (and the scan, whose views all open after it, reads it) or is made after it (and the hook inserts
// it into the new bits). The filter is kReady only after a complete scan.
// The filter must outlive the service and every write transaction (they cache the hook pointer).
class SpecialKeyService {
 public:
  SpecialKeyService(SpecialKeyFilter& filter, Storage& storage, Engine& engine);
  ~SpecialKeyService();  // Stop()
  SpecialKeyService(const SpecialKeyService&) = delete;
  SpecialKeyService& operator=(const SpecialKeyService&) = delete;

  void Start();  // background thread: RebuildNow(filter.Capacity()), then the watch loop
  void Stop();   // sets stop, joins; idempotent
  // engine.Quiesce([&]{ filter.Reset(capacity); }), ScanSpecialKeys, filter.MarkReady(). On a scan
  // error the filter stays kBuilding (SET reads) and the error is returned. Never concurrent with
  // itself: the service thread is its only caller once Start ran.
  Status RebuildNow(size_t capacity);
  uint64_t rebuilds() const;  // completed RebuildNow calls

 private:
  void Run();
  bool WaitFor(std::chrono::milliseconds d);  // false once stop is set
  void NoteSaturation();

  SpecialKeyFilter& filter_;
  Storage& storage_;
  Engine& engine_;
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> rebuilds_{0};
  std::mutex mu_;
  std::condition_variable cv_;
  std::thread thread_;
  bool saturation_warned_ = false;  // service thread only
};

}  // namespace tideskv
