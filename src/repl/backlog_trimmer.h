#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>

#include "common/clock.h"
#include "common/status.h"
#include "exec/repl_log.h"
#include "server/config.h"
#include "storage/compactor.h"
#include "storage/storage.h"

// Backlog trimming (M6a D18): the replication log is bounded by repl-backlog-size (bytes, from the
// batches' cum_bytes) and repl-backlog-ttl (seconds, from their timestamps). Trimming deletes the oldest
// batches with one range delete per transaction (ReplLog::Trim) and, on a cadence, compacts the replog
// CF so that seeks no longer step over the trimmed keys.
namespace tideskv::repl {

inline constexpr uint64_t kNoReplicaNeeds = std::numeric_limits<uint64_t>::max();

struct ReplogHeader {
  int64_t timestamp_ms = 0;
  uint64_t cum_bytes = 0;
};

struct TrimInputs {
  uint64_t first = 1;       // first retained seq
  uint64_t last = 0;        // newest seq
  uint64_t first_cum = 0;   // cum_bytes of replog[first]
  uint64_t first_size = 0;  // encoded size of replog[first]
  int64_t first_ts = 0;     // timestamp_ms of replog[first]
  uint64_t last_cum = 0;    // cum_bytes of replog[last]
  // The oldest seq an attached replica still needs (min of acked+1); kNoReplicaNeeds when none.
  uint64_t oldest_needed = kNoReplicaNeeds;
  int64_t now_ms = 0;
  uint64_t backlog_size = 0;  // bytes
  int64_t backlog_ttl_s = 0;  // 0 = never trim by age
  // The header of replog[seq], first < seq < last; nullopt when it cannot be read. The binary searches
  // call it O(log(last - first)) times.
  std::function<std::optional<ReplogHeader>(uint64_t seq)> peek;
};

// The seq to trim up to (inclusive), or nullopt when nothing is to be trimmed (or a header could not be
// read). Never last or beyond: the newest batch is always kept, so cum_bytes and last survive a restart.
//  - Size: the fewest oldest batches whose deletion leaves at most backlog_size bytes retained.
//  - Age (ttl > 0): every batch older than now - ttl.
//  - An attached replica holds the age trim back to what it still needs, but not the size trim: as with
//    Redis's repl-backlog-ttl, age only frees a backlog nobody uses. A replica that needs a batch the
//    size limit deletes is beyond the limits; the caller drops it before trimming (+FULLRESYNC, M6b).
std::optional<uint64_t> ComputeTrimPoint(const TrimInputs& in);

// Trims the replication log: on a master from its own thread (Start), on a replica from the applier
// thread between batches (RunOnReplica), so a replica's trims never run concurrently with an apply.
class BacklogTrimmer {
 public:
  struct Options {
    std::chrono::milliseconds interval{1000};  // the master thread's tick
    uint64_t max_batches_per_trim = 100000;    // one Trim txn deletes at most this many batches
    uint64_t compact_after_batches = 100000;   // D18: compact once this many were trimmed ...
    std::chrono::milliseconds compact_min_interval{std::chrono::minutes(10)};  // ... and this long after
  };
  struct Hooks {
    std::function<ConfigValues()> config;  // required: repl-backlog-size / repl-backlog-ttl
    // Master only. The oldest seq any attached replica needs (null = kNoReplicaNeeds), and the drop of
    // every replica that needs a seq <= up_to; it must not be called with a lock the manager takes.
    std::function<uint64_t()> oldest_needed;
    std::function<void(uint64_t up_to)> drop_replicas_needing;
  };

  // compactor may be null (no compaction).
  BacklogTrimmer(Storage& storage, ReplLog& log, Compactor* compactor, const Clock& clock, Hooks hooks,
                 Options options);
  ~BacklogTrimmer();  // Stop()
  BacklogTrimmer(const BacklogTrimmer&) = delete;
  BacklogTrimmer& operator=(const BacklogTrimmer&) = delete;

  void Start();  // the master thread; it skips its ticks while local writes are fenced (a replica)
  void Stop();   // idempotent; joins the thread

  // One master round: persists a dormant log's head (ReplLog::PersistHead), computes the trim point, drops
  // replicas beyond the limits, trims, maybe compacts.
  // A no-op once local writes are fenced: the fence (raised before a replica starts applying) is checked
  // under ReplLog::mu_ by every trim txn, so no master trim commits once a replica may apply.
  Status RunOnce();
  // One replica round, on the applier thread between batches. No replica needs this log: a replica
  // refuses PSYNC (M6a has no chained replication, F8) and dropped its replicas when it was demoted.
  Status RunOnReplica();

  uint64_t trimmed_total() const { return trimmed_total_.load(std::memory_order_relaxed); }
  uint64_t compactions() const { return compactions_.load(std::memory_order_relaxed); }  // successful ones
  uint64_t ticks() const { return ticks_.load(std::memory_order_acquire); }  // the thread's finished rounds

 private:
  Status Run(bool master);
  Result<std::optional<uint64_t>> TrimPoint(uint64_t oldest_needed);
  Status TrimTo(uint64_t up_to, bool master);
  void MaybeCompact();
  void Loop();

  Storage& storage_;
  ReplLog& log_;
  Compactor* compactor_;
  const Clock& clock_;
  Hooks hooks_;
  Options options_;

  std::mutex run_mu_;                       // one round at a time (the thread, or the replica's applier)
  uint64_t trimmed_since_compact_ = 0;      // guarded by run_mu_
  std::optional<int64_t> last_compact_ms_;  // guarded by run_mu_
  std::atomic<uint64_t> trimmed_total_{0};
  std::atomic<uint64_t> compactions_{0};
  std::atomic<uint64_t> ticks_{0};

  std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;   // guarded by mu_
  std::mutex join_mu_;  // Stop() from two threads joins once
  std::thread thread_;
};

}  // namespace tideskv::repl
