#include "repl/repl_sender.h"

#include <spdlog/spdlog.h>
#include <sys/socket.h>

#include <algorithm>
#include <utility>

#include "common/debug_delay.h"
#include "common/string_util.h"
#include "encoding/replog_codec.h"
#include "repl/full_sync_frames.h"

namespace tideskv::repl {
namespace {

using namespace std::chrono_literals;

constexpr size_t kCoalesceBytes = size_t{256} * 1024;  // frames per WriteAll
// The watermark wait is sliced so ACKs, EOF and Stop() are noticed within a slice while idle.
constexpr auto kWaitSlice = 50ms;
constexpr auto kPingInterval = 1s;
constexpr auto kDefaultTimeout = 60s;
constexpr int kMaxReadsPerRound = 4;
// What the replica may send: only REPLCONF ACK <seq>. Anything bigger is a protocol error.
constexpr size_t kMaxInput = size_t{64} * 1024;
constexpr resp::ParserLimits kReplicaInputLimits{/*max_bulk_len=*/64, /*max_multibulk_len=*/8,
                                                 /*max_inline_len=*/1024};

int64_t UnixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// replog[S] from the snapshot's view; "" for an empty log (S = 0).
Result<std::string> BaselineRecord(const SyncSnapshot& snap) {
  if (snap.seq == 0) return std::string();
  return snap.view->Get(ColumnFamily::kReplog, encoding::ReplogKey(snap.seq));
}

}  // namespace

std::string BatchFrame(uint64_t seq, std::string_view payload) {
  const std::string s = std::to_string(seq);
  std::string out;
  out.reserve(payload.size() + s.size() + 40);
  out.append("*3\r\n$5\r\nBATCH\r\n$").append(std::to_string(s.size())).append("\r\n").append(s);
  out.append("\r\n$").append(std::to_string(payload.size())).append("\r\n").append(payload).append("\r\n");
  return out;
}

ReplSender::ReplSender(Storage& storage, ReplLog& log, Fd fd, uint64_t next_seq, Callbacks cb)
    : storage_(storage),
      log_(log),
      fd_(std::move(fd)),
      cb_(std::move(cb)),
      start_(std::max<uint64_t>(next_seq, 1)),
      next_(start_),
      parser_(kReplicaInputLimits),
      sent_(next_ - 1) {}

ReplSender::~ReplSender() { Stop(); }

void ReplSender::SetPeer(std::string ip, uint16_t listening_port) {
  peer_ip_ = std::move(ip);
  listening_port_ = listening_port;
}

void ReplSender::SetUnread(std::string bytes) { in_ = std::move(bytes); }

void ReplSender::SetSnapshot(SyncSnapshot snapshot) { snapshot_ = std::move(snapshot); }

void ReplSender::Start() {
  online_.store(!snapshot_.has_value(), std::memory_order_release);
  last_ack_ms_.store(UnixMs(), std::memory_order_release);
  alive_.store(true, std::memory_order_release);
  thread_ = std::thread([this] { Run(); });
}

void ReplSender::Stop() {
  stop_.store(true, std::memory_order_release);
  if (fd_.get() >= 0) ::shutdown(fd_.get(), SHUT_RDWR);  // wakes a poll in WriteAll/ReadSome
  std::lock_guard lock(join_mu_);
  if (thread_.joinable()) thread_.join();
}

std::chrono::milliseconds ReplSender::Timeout() const {
  return cb_.repl_timeout ? cb_.repl_timeout() : std::chrono::milliseconds(kDefaultTimeout);
}

void ReplSender::Run() {
  const Status st = Loop();
  if (!stop_.load(std::memory_order_acquire)) {
    spdlog::warn("replica {}:{} link dropped: {}", peer_ip_, listening_port_, st.ToString());
  }
  ::shutdown(fd_.get(), SHUT_RDWR);  // the replica sees EOF now, not when the manager reaps us
  alive_.store(false, std::memory_order_release);
}

Status ReplSender::Loop() {
  last_write_ = last_ack_ = SteadyClock::now();
  if (snapshot_) {
    if (Status st = StreamSnapshot(); !st.ok()) return st;
  }
  if (Status st = ParseInput(); !st.ok()) return st;  // bytes that followed PSYNC
  while (!stop_.load(std::memory_order_acquire)) {
    if (next_ <= log_.last()) {
      if (Status st = StreamRound(); !st.ok()) return st;
    } else {
      log_.WaitBeyond(next_ - 1, kWaitSlice);
    }
    if (Status st = PollInput(); !st.ok()) return st;
    if (Status st = CheckLiveness(); !st.ok()) return st;
  }
  return Status::Ok();
}

// The full-sync phase (D2, D4, D10): the snapshot's keys as LOAD frames, then LOADEND with replog[S]
// read from the same view. The replica sends nothing meanwhile, so the ACK clock restarts at the end; a
// PING keeps the link alive while keys are slow to read.
Status ReplSender::StreamSnapshot() {
  if (!snapshot_ || !snapshot_->view) return Status::Ok();
  const SyncSnapshot& snap = *snapshot_;
  const Result<uint64_t> ops = StreamChunks(*snap.view);  // its chunker is gone before the view
  if (!ops.ok()) return ops.status();
  const Result<std::string> record = BaselineRecord(snap);
  if (!record.ok()) return record.status();
  const std::string end = LoadEndFrame(snap.seq, *record, *ops);
  if (Status st = WriteAll(fd_.get(), end, Timeout()); !st.ok()) return st;
  snapshot_.reset();  // releases the storage snapshot as soon as it is streamed
  last_write_ = last_ack_ = SteadyClock::now();
  return Status::Ok();
}

// Writes every LOAD frame; returns the number of ops they carried.
Result<uint64_t> ReplSender::StreamChunks(ReadView& view) {
  SnapshotChunker chunker(view, UnixMs());
  const std::function<Status()> between_keys = [this] { return BetweenKeys(); };
  for (;;) {
    auto next = chunker.Next(between_keys);
    if (!next.ok()) return next.status();
    const std::optional<std::string>& chunk = *next;
    if (!chunk.has_value()) return chunker.ops();
    if (Status st = WriteAll(fd_.get(), LoadFrame(*chunk), Timeout()); !st.ok()) return st;
    last_write_ = SteadyClock::now();
  }
}

// Before each snapshot key: Stop() is noticed here, rdb-key-save-delay applies (D14), and a PING goes out
// when nothing was written for kPingInterval.
Status ReplSender::BetweenKeys() {
  if (stop_.load(std::memory_order_acquire)) return Status::IOError("stopped");
  DebugDelay(cb_.key_save_delay_us ? cb_.key_save_delay_us() : 0);
  if (SteadyClock::now() - last_write_ < kPingInterval) return Status::Ok();
  if (Status st = WriteAll(fd_.get(), kPingFrame, Timeout()); !st.ok()) return st;
  last_write_ = SteadyClock::now();
  return Status::Ok();
}

// One round: frames next_.. (at most kCoalesceBytes, never beyond the watermark) and writes them.
Status ReplSender::StreamRound() {
  std::string out;
  if (Status st = ReadFrames(log_.last(), &out); !st.ok()) return st;
  if (Status st = WriteAll(fd_.get(), out, Timeout()); !st.ok()) return st;
  sent_.store(next_ - 1, std::memory_order_release);
  last_write_ = SteadyClock::now();
  return Status::Ok();
}

// A fresh read view per round (an old iterator does not see newer commits), bounded above by the
// watermark `last` and positioned with Seek, never SeekToFirst (tidesdb-replication constraint 8).
Status ReplSender::ReadFrames(uint64_t last, std::string* out) {
  auto view = storage_.BeginRead();
  if (!view.ok()) return view.status();
  auto it = (*view)->NewIterator(ColumnFamily::kReplog,
                                 IterOptions{encoding::ReplogKey(next_), encoding::ReplogKey(last + 1)});
  if (!it.ok()) return it.status();
  Status st = (*it)->Seek(encoding::ReplogKey(next_));
  while (st.ok() && (*it)->Valid() && next_ <= last && out->size() < kCoalesceBytes) {
    uint64_t seq = 0;
    if (!encoding::DecodeReplogKey((*it)->key(), &seq) || seq != next_) break;  // a gap: reported below
    out->append(BatchFrame(seq, (*it)->value()));
    ++next_;
    st = (*it)->Next();
  }
  if (!st.ok() && !st.IsNotFound()) return st;
  if (out->empty()) return Status::NotFound("replog entry " + std::to_string(next_) + " is not retained");
  return Status::Ok();
}

Status ReplSender::PollInput() {
  for (int i = 0; i < kMaxReadsPerRound; ++i) {
    Status st = ReadSome(fd_.get(), &in_, 0ms);
    if (st.code() == StatusCode::kTimedOut) break;
    if (!st.ok()) return st;
    if (Status parsed = ParseInput(); !parsed.ok()) return parsed;
  }
  return Status::Ok();
}

Status ReplSender::ParseInput() {
  size_t offset = 0;
  for (;;) {
    size_t consumed = 0;
    const resp::ParseStatus ps = parser_.Parse(std::string_view(in_).substr(offset), &consumed);
    offset += consumed;
    if (ps == resp::ParseStatus::kNeedMore) break;
    if (ps == resp::ParseStatus::kError)
      return Status::IOError("protocol error from replica: " + parser_.error());
    if (Status st = HandleFrame(parser_.args()); !st.ok()) return st;
  }
  in_.erase(0, offset);
  if (in_.size() + parser_.buffered_bytes() > kMaxInput) return Status::IOError("replica input too large");
  return Status::Ok();
}

// The only frame a replica sends: REPLCONF ACK <seq>, with seq no further than what it was sent.
Status ReplSender::HandleFrame(const std::vector<std::string>& args) {
  uint64_t seq = 0;
  if (args.size() != 3 || !EqualsIgnoreCase(args[0], "replconf") || !EqualsIgnoreCase(args[1], "ack") ||
      !ParseUint64(args[2], &seq)) {
    return Status::IOError("unexpected frame from replica");
  }
  if (seq >= next_) return Status::IOError("replica acknowledged seq " + args[2] + " it was never sent");
  if (seq > acked_.load(std::memory_order_relaxed)) acked_.store(seq, std::memory_order_release);
  last_ack_ = SteadyClock::now();
  last_ack_ms_.store(UnixMs(), std::memory_order_release);
  // Frames are parsed only once LOADEND is written, so any valid ACK means the load is done (PF9: an
  // ACK of 0 after a sync at S = 0 advances nothing but counts).
  online_.store(true, std::memory_order_release);
  if (cb_.on_ack) cb_.on_ack();
  return Status::Ok();
}

// Drops a replica that stopped acknowledging; keeps an idle link alive with PING.
Status ReplSender::CheckLiveness() {
  const auto now = SteadyClock::now();
  const auto timeout = Timeout();
  if (now - last_ack_ > timeout) return Status::TimedOut("no REPLCONF ACK within repl-timeout");
  if (now - last_write_ < kPingInterval) return Status::Ok();
  if (Status st = WriteAll(fd_.get(), kPingFrame, timeout); !st.ok()) return st;
  last_write_ = now;
  return Status::Ok();
}

}  // namespace tideskv::repl
