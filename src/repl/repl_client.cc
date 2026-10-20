#include "repl/repl_client.h"

#include <spdlog/spdlog.h>
#include <sys/socket.h>

#include <algorithm>
#include <utility>

#include "common/string_util.h"
#include "encoding/key_layout.h"
#include "encoding/replog_codec.h"

namespace tideskv::repl {
namespace {

using namespace std::chrono_literals;
using resp::ReplyNode;

// The largest BATCH frame payload accepted from a master is the largest batch a master commits (R12):
// a bigger frame is a protocol error and drops the link, so a hostile or broken master cannot make the
// replica buffer (or decode) more than this. What may sit unparsed: one maximal frame plus its header.
constexpr size_t kMaxInput = encoding::kMaxReplBatchBytes + size_t{64} * 1024;
// The master sends only BATCH <seq> <payload> and PING (D9), and before them, after +FULLRESYNC,
// LOAD <chunk> and LOADEND <seq> <record> <ops> (M6b D4).
constexpr resp::ParserLimits kMasterStreamLimits{/*max_bulk_len=*/encoding::kMaxReplBatchBytes,
                                                 /*max_multibulk_len=*/8, /*max_inline_len=*/1024};

int64_t UnixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::chrono::milliseconds ReplTimeout(const ConfigValues& c) {
  return std::chrono::seconds(std::max<int64_t>(c.repl_timeout, 1));
}

// Redis's replica accepts these PING errors: the master wants AUTH first, or the user may not PING.
bool PingErrorAccepted(std::string_view err) {
  return err.starts_with("NOAUTH") || err.starts_with("NOPERM") ||
         err.starts_with("ERR operation not permitted");
}

}  // namespace

ReplClient::ReplClient(Engine& engine, ReplLog& log, ConfigSnapshotFn config, Callbacks cb)
    : ReplClient(engine, log, std::move(config), std::move(cb), Options{}) {}

ReplClient::ReplClient(Engine& engine, ReplLog& log, ConfigSnapshotFn config, Callbacks cb, Options options)
    : engine_(engine),
      log_(log),
      config_(std::move(config)),
      cb_(std::move(cb)),
      options_(options),
      parser_(kMasterStreamLimits),
      baseline_unknown_(options.baseline_unknown) {}

ReplClient::~ReplClient() { Stop(); }

void ReplClient::Start(std::string host, uint16_t port) {
  std::lock_guard lock(join_mu_);
  if (thread_.joinable() || stopping()) return;
  host_ = std::move(host);
  port_ = port;
  thread_ = std::thread([this] { Run(); });
}

void ReplClient::Stop() {
  stop_.store(true, std::memory_order_release);
  {
    std::lock_guard lock(fd_mu_);
    if (active_fd_ >= 0) ::shutdown(active_fd_, SHUT_RDWR);  // wakes a poll in ReadSome/WriteAll
  }
  { std::lock_guard lock(backoff_mu_); }  // a waiter is either before its predicate check or waiting
  backoff_cv_.notify_all();
  std::lock_guard lock(join_mu_);
  if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
}

void ReplClient::SetState(ReplicaLinkState s) {
  const ReplicaLinkState prev = state_.exchange(s, std::memory_order_acq_rel);
  if (prev == s) return;
  if (s == ReplicaLinkState::kConnected) {
    link_down_since_ms_.store(0, std::memory_order_release);
  } else if (prev == ReplicaLinkState::kConnected) {
    link_down_since_ms_.store(UnixMs(), std::memory_order_release);
  }
  if (cb_.on_state) cb_.on_state(s);
}

void ReplClient::WaitBackoff(std::chrono::milliseconds d) {
  std::unique_lock lock(backoff_mu_);
  backoff_cv_.wait_for(lock, d, [this] { return stopping(); });
}

// Connects, streams until the link drops, then waits 1, 2, 4 ... 30 s (D19); a link that got +CONTINUE,
// or completed a full sync (M6b D17), starts the backoff over.
void ReplClient::Run() {
  auto backoff = options_.backoff_initial;
  while (!stopping()) {
    if (!DiscardUnfinishedLoad(backoff)) {
      WaitBackoff(backoff);
      backoff = std::min(backoff * 2, options_.backoff_max);
      continue;
    }
    bool continued = false;
    const Status st = RunLink(&continued);
    SetState(ReplicaLinkState::kConnect);
    if (stopping()) break;
    if (continued) backoff = options_.backoff_initial;
    spdlog::warn("replication from {}:{} stopped: {}; retrying in {} ms", host_, port_, st.ToString(),
                 backoff.count());
    WaitBackoff(backoff);
    backoff = std::min(backoff * 2, options_.backoff_max);
  }
}

// G1 (M6b final review #1): the loader only logs a failed abort, which leaves the node LOADING with the
// marker set. No PSYNC may follow: "PSYNC ? 1" answered +CONTINUE would stream batches onto the partial
// keyspace. So the abort is retried here, with the backoff, before any reconnect; meanwhile the node
// stays LOADING.
bool ReplClient::DiscardUnfinishedLoad(std::chrono::milliseconds retry_in) {
  if (!engine_.loading() && !log_.load_incomplete()) return true;
  const Status st = engine_.AbortLoad();
  if (st.ok()) {
    spdlog::warn("a partial full-sync load from {}:{} is discarded", host_, port_);
    return true;
  }
  spdlog::error("cannot discard a partial full-sync load: {}; retrying in {} ms before reconnecting",
                st.ToString(), retry_in.count());
  return false;
}

Status ReplClient::RunLink(bool* continued) {
  const ConfigValues cfg = config_();
  timeout_ = ReplTimeout(cfg);
  key_load_delay_.store(cfg.key_load_delay, std::memory_order_relaxed);
  in_.clear();
  parser_ = resp::RequestParser(kMasterStreamLimits);
  SetState(ReplicaLinkState::kConnecting);
  auto fd = ConnectTcp(host_, port_, timeout_, &stop_);
  if (!fd.ok()) return fd.status();
  if (!Publish(fd->get())) return Status::IOError("stopped");
  struct Unpublisher {
    ReplClient* client;
    ~Unpublisher() { client->Unpublish(); }  // before fd closes: Stop never shuts down a reused number
  } const unpublish{this};
  RespConn conn(fd->get(), &in_, timeout_);  // what follows +CONTINUE stays in in_ for Stream (M7b D3)
  SetState(ReplicaLinkState::kHandshake);
  if (Status st = Handshake(conn, cfg); !st.ok()) return st;
  PsyncOutcome psync;
  if (Status st = Psync(conn, &psync); !st.ok()) return st;
  if (psync.full) {
    if (Status st = FullSync(fd->get(), psync); !st.ok()) return st;
  }
  *continued = true;  // CONTINUE or a completed full sync resets the backoff (D17)
  SetState(ReplicaLinkState::kConnected);
  spdlog::info("replicating from {}:{}: {} at seq {}", host_, port_,
               psync.full ? "full resynchronisation loaded, streaming" : "partial resynchronisation accepted",
               log_.last() + 1);
  return Stream(fd->get());
}

bool ReplClient::Publish(int fd) {
  std::lock_guard lock(fd_mu_);
  if (stopping()) return false;
  active_fd_ = fd;
  return true;
}

void ReplClient::Unpublish() {
  std::lock_guard lock(fd_mu_);
  active_fd_ = -1;
}

// PING, AUTH, REPLCONF listening-port / ip-address / capa (D8). Only PING and AUTH must succeed; a master
// that does not understand a REPLCONF option is logged and tolerated, as Redis does.
Status ReplClient::Handshake(RespConn& conn, const ConfigValues& cfg) {
  auto pong = conn.Command({"PING"});
  if (!pong.ok()) return pong.status();
  if (pong->type == ReplyNode::Type::kError && !PingErrorAccepted(pong->str)) {
    return Status::IOError("the master answered PING with -" + pong->str);
  }
  if (!cfg.masterauth.empty()) {
    std::vector<std::string_view> auth{"AUTH"};
    if (!cfg.masteruser.empty()) auth.emplace_back(cfg.masteruser);
    auth.emplace_back(cfg.masterauth);
    auto r = conn.Command(auth);
    if (!r.ok()) return r.status();
    if (r->type == ReplyNode::Type::kError) return Status::IOError("the master refused AUTH: -" + r->str);
  }
  const std::string port =
      std::to_string(cfg.replica_announce_port != 0 ? cfg.replica_announce_port : cfg.port);
  std::vector<std::vector<std::string_view>> options = {{"REPLCONF", "listening-port", port}};
  if (!cfg.replica_announce_ip.empty())
    options.push_back({"REPLCONF", "ip-address", cfg.replica_announce_ip});
  options.push_back({"REPLCONF", "capa", "psync2"});
  for (const auto& option : options) {
    auto r = conn.Command(option);
    if (!r.ok()) return r.status();
    if (r->type == ReplyNode::Type::kError) {
      spdlog::warn("master {}:{} refused REPLCONF {}: -{}", host_, port_, option[1], r->str);
    }
  }
  return ExchangeKeyLayouts(conn, cfg);
}

// Slotless key layout: REPLCONF key-layout <mine> is answered with the master's layout, or -ERR by a master
// that predates layouts (slotted). Decisive, unlike the options above: a cluster node is slotted and cannot
// follow a slotless master, so it stops here, before PSYNC and so before any full sync wipes its data. Any
// other layout difference is resolved by a full resynchronisation that adopts the master's layout.
Status ReplClient::ExchangeKeyLayouts(RespConn& conn, const ConfigValues& cfg) {
  const encoding::KeyLayout mine = encoding::CurrentKeyLayout();
  auto r = conn.Command({"REPLCONF", "key-layout", encoding::KeyLayoutName(mine)});
  if (!r.ok()) return r.status();
  if (r->type == ReplyNode::Type::kError) {
    master_layout_ = encoding::KeyLayout::kSlotted;
  } else {
    const std::optional<encoding::KeyLayout> theirs =
        r->type == ReplyNode::Type::kStatus ? encoding::ParseKeyLayout(r->str) : std::nullopt;
    if (!theirs) return Status::IOError("protocol error: unexpected reply to REPLCONF key-layout");
    master_layout_ = *theirs;
  }
  if (cfg.cluster_enabled && master_layout_ == encoding::KeyLayout::kSlotless) {
    return Status::InvalidArgument(
        "the master uses the standalone key layout (format 2), and cluster mode needs the slotted layout "
        "(format 1): this replica does not synchronise from it");
  }
  if (master_layout_ != mine) {
    spdlog::warn(
        "master {}:{} uses the {} key layout and this replica the {} one: a full resynchronisation "
        "will replace this replica's data and adopt the master's layout",
        host_, port_, encoding::KeyLayoutName(master_layout_), encoding::KeyLayoutName(mine));
  }
  return Status::Ok();
}

// PSYNC <replid|?> <last+1> (D8). OK for +CONTINUE (the replid adopted) and for a valid
// +FULLRESYNC <replid> <seq> (out->full; the load follows on this link, M6b D4).
Status ReplClient::Psync(RespConn& conn, PsyncOutcome* out) {
  const uint64_t last = log_.last();
  const bool unknown = baseline_unknown_.load(std::memory_order_acquire);
  std::string replid = last == 0 && !unknown ? "?" : cb_.identity().replid;  // F5
  // Another key layout: no history of the master can describe this data, and a master that predates layouts
  // could still answer "?" with +CONTINUE. An id no node has (40 zeros) always gets +FULLRESYNC.
  const bool other_layout = master_layout_ != encoding::CurrentKeyLayout();
  if (other_layout) replid = std::string(kReplIdLength, '0');
  const std::string next = std::to_string(last + 1);
  auto r = conn.Command({"PSYNC", replid, next});
  if (!r.ok()) return r.status();
  if (r->type == ReplyNode::Type::kError) return Status::IOError("the master refused PSYNC: -" + r->str);
  const std::string_view reply = r->str;
  if (r->type == ReplyNode::Type::kStatus && reply.starts_with("CONTINUE ")) {
    if (other_layout) return Status::IOError("protocol error: +CONTINUE from a master of another key layout");
    return AdoptReplid(reply.substr(9));
  }
  if (r->type == ReplyNode::Type::kStatus && reply.starts_with("FULLRESYNC ")) {
    const std::string_view rest = reply.substr(11);
    const size_t space = rest.find(' ');
    if (space == std::string_view::npos || !IsValidReplId(rest.substr(0, space))) {
      return Status::IOError("protocol error: invalid replid in +FULLRESYNC");
    }
    if (!ParseUint64(rest.substr(space + 1), &out->seq)) {
      return Status::IOError("protocol error: invalid seq in +FULLRESYNC");
    }
    out->full = true;
    out->replid = std::string(rest.substr(0, space));
    return Status::Ok();
  }
  return Status::IOError("protocol error: unexpected PSYNC reply");
}

// +FULLRESYNC (M6b D4, D5): loads the snapshot that follows on this link, in place. Any failure (the
// link dropping, a protocol error, Stop) destroys the loader, which discards the partial load.
Status ReplClient::FullSync(int fd, const PsyncOutcome& o) {
  SetState(ReplicaLinkState::kSync);
  spdlog::info("full resynchronisation from {}:{} at seq {}", host_, port_, o.seq);
  sync_read_bytes_.store(in_.size(), std::memory_order_release);  // what followed the +FULLRESYNC line
  sync_last_io_ms_.store(UnixMs(), std::memory_order_release);
  struct EndProgress {
    ReplClient* c;
    ~EndProgress() { c->ResetSyncProgress(); }
  } const end_progress{this};
  // Before Begin: BeginLoad raises Engine::loading(), and INFO must never show loading:1 with no start time.
  loading_start_ms_.store(UnixMs(), std::memory_order_release);
  std::optional<encoding::KeyLayout> adopt;
  if (master_layout_ != encoding::CurrentKeyLayout()) adopt = master_layout_;
  FullSyncLoader loader(engine_, o.replid, o.seq, LoaderHooks(), adopt);
  if (Status st = loader.Begin(); !st.ok()) return st;
  struct MidLoad {  // D22: a Stop between the start txn and LOADEND leaves the keyspace wiped or partial
    ReplClient* c;
    bool done = false;
    ~MidLoad() {
      if (!done && c->stopping()) c->stopped_mid_load_.store(true, std::memory_order_release);
    }
  } mid_load{this};
  if (Status st = LoadUntilDone(fd, loader); !st.ok()) return st;
  mid_load.done = true;
  spdlog::info("full resynchronisation from {}:{} loaded: {} bytes, seq {}", host_, port_,
               loader.loaded_bytes(), o.seq);
  return Status::Ok();
}

// Reads and loads until LOADEND committed. The master PINGs an idle link, so repl-timeout of silence drops
// it (D10). The bytes after LOADEND stay in in_ for Stream.
Status ReplClient::LoadUntilDone(int fd, FullSyncLoader& loader) {
  bool done = false;
  if (Status st = ParseLoadFrames(loader, &done); !st.ok()) return st;
  auto last_io = SteadyClock::now();
  while (!done) {
    if (stopping()) return Status::IOError("stopped");
    const size_t before = in_.size();
    Status st = ReadSome(fd, &in_, std::min(options_.ack_interval, timeout_));
    const auto now = SteadyClock::now();
    if (st.ok()) {
      last_io = now;
      sync_read_bytes_.fetch_add(in_.size() - before, std::memory_order_acq_rel);
      sync_last_io_ms_.store(UnixMs(), std::memory_order_release);
      if (Status parsed = ParseLoadFrames(loader, &done); !parsed.ok()) return parsed;
      continue;
    }
    if (st.code() != StatusCode::kTimedOut) return st;
    if (now - last_io >= timeout_) {
      return Status::TimedOut("no data from the master within repl-timeout during a full sync");
    }
  }
  return Status::Ok();
}

Status ReplClient::ParseLoadFrames(FullSyncLoader& loader, bool* done) {
  const auto handle = [&](const std::vector<std::string>& args) {
    if (stopping()) return Status::IOError("stopped");  // a delayed load stops between frames too (PF14)
    return loader.HandleFrame(args, done);
  };
  const Status st = DrainFrames(handle, done);
  loaded_bytes_.store(loader.loaded_bytes(), std::memory_order_release);
  return st;
}

FullSyncLoader::Hooks ReplClient::LoaderHooks() {
  FullSyncLoader::Hooks h;
  h.identity = cb_.identity;
  h.publish_identity = [this](const ReplIdentity& id) {
    if (cb_.publish_identity) cb_.publish_identity(id);
  };
  h.on_baseline_known = [this] {
    baseline_unknown_.store(false, std::memory_order_release);
    if (cb_.on_baseline_known) cb_.on_baseline_known();
  };
  h.key_load_delay_us = [this] { return key_load_delay_.load(std::memory_order_relaxed); };
  h.stopping = [this] { return stopping(); };
  return h;
}

void ReplClient::ResetSyncProgress() {
  sync_read_bytes_.store(0, std::memory_order_release);
  sync_last_io_ms_.store(0, std::memory_order_release);
  loading_start_ms_.store(0, std::memory_order_release);
  loaded_bytes_.store(0, std::memory_order_release);
}

// +CONTINUE <id> (D7, CF5): an id that differs from ours is validated, then adopted and persisted here,
// on the only committer, before the first batch is applied.
Status ReplClient::AdoptReplid(std::string_view replid) {
  if (!IsValidReplId(replid)) return Status::IOError("protocol error: invalid replid in +CONTINUE");
  const ReplIdentity me = cb_.identity();
  if (replid == me.replid) return Status::Ok();
  if (Status st = cb_.persist_identity(Adopt(me, replid, log_.last())); !st.ok()) return st;
  spdlog::info("replica adopted the master's replication id {} at seq {}", replid, log_.last());
  if (cb_.on_adopt) cb_.on_adopt(std::string(replid));
  return Status::Ok();
}

// Applies frames as they arrive and acknowledges them (D9, D19). A link silent for repl-timeout drops.
Status ReplClient::Stream(int fd) {
  last_io_ = last_ack_ = SteadyClock::now();
  last_io_ms_.store(UnixMs(), std::memory_order_release);
  ack_due_ = true;                                     // tell the master where we are at once
  if (Status st = ParseFrames(); !st.ok()) return st;  // bytes that followed +CONTINUE
  while (!stopping()) {
    if (Status st = MaybeAck(fd); !st.ok()) return st;
    MaybeTrim();  // an idle link trims too (by age)
    Status st = ReadSome(fd, &in_, std::min(options_.ack_interval, timeout_));
    const auto now = SteadyClock::now();
    if (st.ok()) {
      last_io_ = now;
      last_io_ms_.store(UnixMs(), std::memory_order_release);
      if (Status parsed = ParseFrames(); !parsed.ok()) return parsed;
      continue;
    }
    if (st.code() != StatusCode::kTimedOut) return st;
    if (now - last_io_ >= timeout_) return Status::TimedOut("no data from the master within repl-timeout");
  }
  return Status::Ok();
}

Status ReplClient::DrainFrames(const FrameHandler& handle, const bool* stop) {
  size_t offset = 0;
  while (stop == nullptr || !*stop) {
    size_t consumed = 0;
    const resp::ParseStatus ps = parser_.Parse(std::string_view(in_).substr(offset), &consumed);
    offset += consumed;
    if (ps == resp::ParseStatus::kNeedMore) break;
    if (ps == resp::ParseStatus::kError)
      return Status::IOError("protocol error from master: " + parser_.error());
    if (Status st = handle(parser_.args()); !st.ok()) return st;
  }
  in_.erase(0, offset);
  if (in_.size() + parser_.buffered_bytes() > kMaxInput) return Status::IOError("master input too large");
  return Status::Ok();
}

Status ReplClient::ParseFrames() {
  if (Status st =
          DrainFrames([this](const std::vector<std::string>& args) { return HandleFrame(args); }, nullptr);
      !st.ok()) {
    return st;
  }
  const bool drained = in_.empty() && parser_.buffered_bytes() == 0;
  if (drained && log_.last() != acked_) ack_due_ = true;  // caught up: acknowledge right away
  return Status::Ok();
}

// BATCH <seq> <payload> must continue our log exactly (D9 strict seq); PING asks for an ACK.
Status ReplClient::HandleFrame(const std::vector<std::string>& args) {
  if (args.size() == 1 && EqualsIgnoreCase(args[0], "ping")) {
    ack_due_ = true;
    return Status::Ok();
  }
  uint64_t seq = 0;
  if (args.size() != 3 || !EqualsIgnoreCase(args[0], "batch") || !ParseUint64(args[1], &seq)) {
    return Status::IOError("protocol error: unexpected frame from master");
  }
  const uint64_t expected = log_.last() + 1;
  if (seq != expected) {
    return Status::IOError("gap in the master's stream: got seq " + std::to_string(seq) + ", expected " +
                           std::to_string(expected));
  }
  if (Status st = engine_.ApplyReplicated(seq, args[2]); !st.ok()) {
    spdlog::error("cannot apply replicated batch {}: {}", seq, st.ToString());
    return st;
  }
  MaybeTrim();  // between batches: the apply has committed, the next has not begun (CF10)
  return Status::Ok();
}

// D18 on a replica: the trim runs on this thread, the only committer, so never during an apply.
void ReplClient::MaybeTrim() {
  if (!cb_.trim_backlog) return;
  const auto now = SteadyClock::now();
  if (now - last_trim_ < options_.trim_interval) return;
  last_trim_ = now;
  if (Status st = cb_.trim_backlog(); !st.ok())
    spdlog::warn("replica backlog trim failed: {}", st.ToString());
}

// Exactly `REPLCONF ACK <seq>` (CF12): every ack_interval, after a PING, and on catching up.
Status ReplClient::MaybeAck(int fd) {
  const auto now = SteadyClock::now();
  if (!ack_due_ && now - last_ack_ < options_.ack_interval) return Status::Ok();
  const uint64_t seq = log_.last();
  const std::string seq_text = std::to_string(seq);
  if (Status st = WriteAll(fd, RespCommand({"REPLCONF", "ACK", seq_text}), timeout_); !st.ok()) return st;
  acked_ = seq;
  ack_due_ = false;
  last_ack_ = now;
  return Status::Ok();
}

}  // namespace tideskv::repl
