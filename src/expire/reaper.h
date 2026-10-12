#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "common/status.h"
#include "exec/engine.h"

namespace tideskv {

// Active expiry (spec §4.5): deletes objects whose deadline has passed, through Engine
// like any write command. Reads already hide expired objects; this reclaims them.
// An index entry is trusted only if it still matches meta (same expire_ms and oid);
// otherwise it is stale (spec §4.5: harmless by design) and only the entry is dropped.
// M6: runs on the master only.
//
// M2 simplifications:
//  - A db whose batch came back full is simply processed again until the cycle budget is
//    spent; spec §4.5's ">25% of the batch was due" heuristic is deferred to M8 tuning.
//  - A batch's deletes are split into Engine::Write calls of kKeysPerWrite keys rather
//    than one transaction per batch (see kKeysPerWrite).
//  - expired_total() (INFO expired_keys) counts reaper deletions only, not keys that
//    writers drop lazily through LoadForWrite.
class ExpireReaper {
 public:
  struct Options {
    std::chrono::milliseconds interval{100};     // spec §4.5
    size_t batch_size = 1000;                    // spec §4.5
    std::chrono::milliseconds cycle_budget{25};  // keep re-running full batches until this is spent
  };

  ExpireReaper(Engine& engine, Options options) : engine_(engine), options_(options) {}
  ~ExpireReaper() { Stop(); }
  ExpireReaper(const ExpireReaper&) = delete;
  ExpireReaper& operator=(const ExpireReaper&) = delete;

  // One cycle over all dbs; returns the number of objects deleted. Every db gets at least
  // one batch, even with a zero budget. Must not be called from inside an Engine fn.
  Result<size_t> RunOnce();
  // Start and Stop must not be called concurrently with each other (App calls them from one thread).
  void Start();
  void Stop();  // idempotent
  uint64_t expired_total() const { return expired_total_.load(std::memory_order_relaxed); }

 private:
  // A batch is deleted in Engine::Write calls of at most this many keys, so the reaper never
  // holds more than 32 key stripes at once: client writes are not stalled behind a
  // 1000-key transaction, and ThreadSanitizer (which aborts above 64 held mutexes per
  // thread) can check it. Each entry is still re-checked against meta under its key lock.
  static constexpr size_t kKeysPerWrite = 32;

  struct Due {
    std::string key;
    uint64_t expire_ms;
    uint64_t oid;
  };
  Status CollectDue(uint8_t db, int64_t now_ms, std::vector<Due>* out);
  Result<size_t> ReapBatch(uint8_t db, int64_t now_ms, bool* full);
  Result<size_t> DeleteDue(uint8_t db, std::span<const Due> due);
  void Loop();

  Engine& engine_;
  Options options_;
  std::atomic<uint64_t> expired_total_{0};
  std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::thread thread_;
};

}  // namespace tideskv
