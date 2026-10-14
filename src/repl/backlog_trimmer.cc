#include "repl/backlog_trimmer.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>
#include <utility>

#include "encoding/replog_codec.h"

namespace tideskv::repl {
namespace {

// Reads headers for the binary searches; remembers whether any read failed.
class HeaderReader {
 public:
  explicit HeaderReader(const TrimInputs& in) : in_(in) {}
  std::optional<ReplogHeader> At(uint64_t seq) {
    if (seq == in_.first) return ReplogHeader{in_.first_ts, in_.first_cum};
    std::optional<ReplogHeader> h = in_.peek ? in_.peek(seq) : std::nullopt;
    if (!h) failed_ = true;
    return h;
  }
  bool failed() const { return failed_; }

 private:
  const TrimInputs& in_;
  bool failed_ = false;
};

// The smallest t in [first, last - 1] whose deletion (with everything before it) leaves at most
// backlog_size bytes: last_cum - cum(t) <= backlog_size; last - 1 when even that is too much.
// first - 1 when the retained bytes already fit.
uint64_t SizeTrimPoint(const TrimInputs& in, HeaderReader& reader) {
  const uint64_t before_first = in.first_cum >= in.first_size ? in.first_cum - in.first_size : 0;
  const auto fits = [&](uint64_t cum) { return cum >= in.last_cum || in.last_cum - cum <= in.backlog_size; };
  if (fits(before_first)) return in.first - 1;
  uint64_t lo = in.first;
  uint64_t hi = in.last - 1;
  while (lo < hi) {
    const uint64_t mid = lo + (hi - lo) / 2;
    const std::optional<ReplogHeader> h = reader.At(mid);
    if (!h) return in.first - 1;
    if (fits(h->cum_bytes)) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return lo;
}

// The largest t in [first, last - 1] older than now - ttl; first - 1 when ttl is 0 or none is.
uint64_t AgeTrimPoint(const TrimInputs& in, HeaderReader& reader) {
  if (in.backlog_ttl_s <= 0) return in.first - 1;
  constexpr int64_t kMaxSeconds = std::numeric_limits<int64_t>::max() / 1000;
  const int64_t ttl_ms =
      in.backlog_ttl_s > kMaxSeconds ? std::numeric_limits<int64_t>::max() : in.backlog_ttl_s * 1000;
  if (in.now_ms < std::numeric_limits<int64_t>::min() + ttl_ms) return in.first - 1;
  const int64_t cutoff = in.now_ms - ttl_ms;
  if (in.first_ts >= cutoff) return in.first - 1;
  uint64_t lo = in.first;
  uint64_t hi = in.last - 1;
  while (lo < hi) {
    const uint64_t mid = lo + (hi - lo + 1) / 2;
    const std::optional<ReplogHeader> h = reader.At(mid);
    if (!h) return in.first - 1;
    if (h->timestamp_ms < cutoff) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  return lo;
}

}  // namespace

std::optional<uint64_t> ComputeTrimPoint(const TrimInputs& in) {
  if (in.last == 0 || in.first == 0 || in.first >= in.last) return std::nullopt;  // only the newest left
  HeaderReader reader(in);
  const uint64_t by_size = SizeTrimPoint(in, reader);
  const uint64_t by_age = AgeTrimPoint(in, reader);
  const uint64_t held = in.oldest_needed == 0 ? 0 : in.oldest_needed - 1;  // what replicas let go
  const uint64_t up_to = std::max(by_size, std::min(by_age, held));
  if (reader.failed() || up_to < in.first) return std::nullopt;
  return up_to;
}

BacklogTrimmer::BacklogTrimmer(Storage& storage, ReplLog& log, Compactor* compactor, const Clock& clock,
                               Hooks hooks, Options options)
    : storage_(storage),
      log_(log),
      compactor_(compactor),
      clock_(clock),
      hooks_(std::move(hooks)),
      options_(options) {}

BacklogTrimmer::~BacklogTrimmer() { Stop(); }

void BacklogTrimmer::Start() {
  std::lock_guard lock(mu_);
  if (stop_ || thread_.joinable()) return;
  thread_ = std::thread([this] { Loop(); });
}

void BacklogTrimmer::Stop() {
  {
    std::lock_guard lock(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  std::lock_guard join(join_mu_);
  if (thread_.joinable()) thread_.join();
}

void BacklogTrimmer::Loop() {
  std::unique_lock lock(mu_);
  while (!stop_) {
    if (cv_.wait_for(lock, options_.interval, [this] { return stop_; })) break;
    lock.unlock();
    if (Status st = RunOnce(); !st.ok()) spdlog::warn("replication backlog trim failed: {}", st.ToString());
    ticks_.fetch_add(1, std::memory_order_release);
    lock.lock();
  }
}

Status BacklogTrimmer::RunOnce() { return Run(/*master=*/true); }

Status BacklogTrimmer::RunOnReplica() { return Run(/*master=*/false); }

Status BacklogTrimmer::Run(bool master) {
  std::lock_guard lock(run_mu_);
  if (master && log_.fenced()) return Status::Ok();  // a replica: its applier trims
  const uint64_t needed = master && hooks_.oldest_needed ? hooks_.oldest_needed() : kNoReplicaNeeds;
  auto point = TrimPoint(needed);
  if (!point.ok()) return point.status();
  if (const std::optional<uint64_t> up_to = *point; up_to.has_value()) {
    // Replicas that need what is about to go are beyond the limits: dropped first, so no sender is
    // reading a batch the trim deletes (it would drop its link anyway, on the gap). A replica that
    // attaches meanwhile, from a PSYNC decided before the trim, drops its link on that gap and comes
    // back to a +FULLRESYNC.
    if (needed <= *up_to && hooks_.drop_replicas_needing) hooks_.drop_replicas_needing(*up_to);
    if (Status st = TrimTo(*up_to, master); !st.ok()) return st;
  }
  MaybeCompact();
  return Status::Ok();
}

// The inputs of ComputeTrimPoint, read from one view; the binary searches peek at more headers.
Result<std::optional<uint64_t>> BacklogTrimmer::TrimPoint(uint64_t oldest_needed) {
  const uint64_t first = log_.first_retained();
  const uint64_t last = log_.last();
  if (last == 0 || first >= last) return std::optional<uint64_t>();
  auto view = storage_.BeginRead();
  if (!view.ok()) return view.status();
  ReadView& v = **view;
  const auto header = [&](uint64_t seq, ReplogHeader* h, size_t* size) -> Status {
    auto raw = v.Get(ColumnFamily::kReplog, encoding::ReplogKey(seq));
    if (!raw.ok()) return raw.status();
    if (!encoding::PeekReplogHeader(*raw, &h->timestamp_ms, &h->cum_bytes)) {
      return Status::Corruption("replog entry " + std::to_string(seq) + " has no valid header");
    }
    if (size != nullptr) *size = raw->size();
    return Status::Ok();
  };
  ReplogHeader first_h;
  ReplogHeader last_h;
  size_t first_size = 0;
  if (Status st = header(first, &first_h, &first_size); !st.ok()) return st;
  if (Status st = header(last, &last_h, nullptr); !st.ok()) return st;
  const ConfigValues cfg = hooks_.config();
  TrimInputs in;
  in.first = first;
  in.last = last;
  in.first_cum = first_h.cum_bytes;
  in.first_size = first_size;
  in.first_ts = first_h.timestamp_ms;
  in.last_cum = last_h.cum_bytes;
  in.oldest_needed = oldest_needed;
  in.now_ms = clock_.NowMs();
  in.backlog_size = cfg.repl_backlog_size;
  in.backlog_ttl_s = cfg.repl_backlog_ttl;
  in.peek = [&](uint64_t seq) -> std::optional<ReplogHeader> {
    ReplogHeader h;
    if (!header(seq, &h, nullptr).ok()) return std::nullopt;
    return h;
  };
  return ComputeTrimPoint(in);
}

// Steps of at most max_batches_per_trim, one txn each. The master's steps stop once local writes are
// fenced (a demotion raced this round).
Status BacklogTrimmer::TrimTo(uint64_t up_to, bool master) {
  const uint64_t step = std::max<uint64_t>(options_.max_batches_per_trim, 1);
  for (uint64_t first = log_.first_retained(); first <= up_to; first = log_.first_retained()) {
    const uint64_t to = std::min(up_to, first + step - 1);
    bool trimmed = true;
    Status st = master ? log_.TrimUnlessFenced(to, &trimmed) : log_.Trim(to);
    if (!st.ok()) return st;
    if (!trimmed) return Status::Ok();
    trimmed_since_compact_ += to - first + 1;
    trimmed_total_.fetch_add(to - first + 1, std::memory_order_relaxed);
  }
  return Status::Ok();
}

// D18: a whole-CF compaction of the replog once enough was trimmed, at most once per interval. A busy CF
// (an engine compaction holds it) or a failure is retried on a later round.
void BacklogTrimmer::MaybeCompact() {
  if (compactor_ == nullptr || trimmed_since_compact_ == 0 ||
      trimmed_since_compact_ < options_.compact_after_batches) {
    return;
  }
  const int64_t now = clock_.NowMs();
  if (last_compact_ms_ && now - *last_compact_ms_ < options_.compact_min_interval.count()) return;
  Status st = compactor_->Checkpoint();  // range tombstones still in the memtable are not compacted
  if (st.ok()) st = compactor_->Compact(ColumnFamily::kReplog);
  if (!st.ok()) {
    if (st.code() != StatusCode::kBusy) spdlog::warn("replication log compaction failed: {}", st.ToString());
    return;
  }
  spdlog::info("compacted the replication log after trimming {} batches", trimmed_since_compact_);
  last_compact_ms_ = now;
  trimmed_since_compact_ = 0;
  compactions_.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace tideskv::repl
