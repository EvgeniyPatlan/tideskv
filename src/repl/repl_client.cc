#include "repl/repl_client.h"

#include <spdlog/spdlog.h>
#include <sys/socket.h>

#include <algorithm>
#include <utility>

#include "common/string_util.h"
#include "encoding/replog_codec.h"

namespace tideskv::repl {
namespace {

using namespace std::chrono_literals;
using resp::ReplyNode;

constexpr size_t kMaxReplyLine = size_t{64} * 1024;
// The largest BATCH frame payload accepted from a master is the largest batch a master commits (R12):
// a bigger frame is a protocol error and drops the link, so a hostile or broken master cannot make the
// replica buffer (or decode) more than this. What may sit unparsed: one maximal frame plus its header.
constexpr size_t kMaxInput = encoding::kMaxReplBatchBytes + size_t{64} * 1024;
// The master sends only BATCH <seq> <payload> and PING (D9).
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
      parser_(kMasterStreamLimits) {}

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

// Connects, streams until the link drops, then waits 1, 2, 4 ... 30 s (D19); a link that got +CONTINUE
// starts the backoff over.
void ReplClient::Run() {
  auto backoff = options_.backoff_initial;
  while (!stopping()) {
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

Status ReplClient::RunLink(bool* continued) {
  const ConfigValues cfg = config_();
  timeout_ = ReplTimeout(cfg);
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
  SetState(ReplicaLinkState::kHandshake);
  if (Status st = Handshake(fd->get(), cfg); !st.ok()) return st;
  if (Status st = Psync(fd->get(), continued); !st.ok()) return st;
  SetState(ReplicaLinkState::kConnected);
  spdlog::info("replicating from {}:{}: partial resynchronisation accepted at seq {}", host_, port_,
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
Status ReplClient::Handshake(int fd, const ConfigValues& cfg) {
  auto pong = Command(fd, {"PING"});
  if (!pong.ok()) return pong.status();
  if (pong->type == ReplyNode::Type::kError && !PingErrorAccepted(pong->str)) {
    return Status::IOError("the master answered PING with -" + pong->str);
  }
  if (!cfg.masterauth.empty()) {
    std::vector<std::string_view> auth{"AUTH"};
    if (!cfg.masteruser.empty()) auth.emplace_back(cfg.masteruser);
    auth.emplace_back(cfg.masterauth);
    auto r = Command(fd, auth);
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
    auto r = Command(fd, option);
    if (!r.ok()) return r.status();
    if (r->type == ReplyNode::Type::kError) {
      spdlog::warn("master {}:{} refused REPLCONF {}: -{}", host_, port_, option[1], r->str);
    }
  }
  return Status::Ok();
}

// PSYNC <replid|?> <last+1> (D8). OK only for +CONTINUE: *continued is set and streaming may begin.
Status ReplClient::Psync(int fd, bool* continued) {
  const uint64_t last = log_.last();
  const std::string replid = last == 0 ? "?" : cb_.identity().replid;
  const std::string next = std::to_string(last + 1);
  auto r = Command(fd, {"PSYNC", replid, next});
  if (!r.ok()) return r.status();
  if (r->type == ReplyNode::Type::kError) return Status::IOError("the master refused PSYNC: -" + r->str);
  const std::string_view reply = r->str;
  if (r->type == ReplyNode::Type::kStatus && reply.starts_with("CONTINUE ")) {
    if (Status st = AdoptReplid(reply.substr(9)); !st.ok()) return st;
    *continued = true;
    return Status::Ok();
  }
  if (r->type == ReplyNode::Type::kStatus && reply.starts_with("FULLRESYNC ")) {
    const std::string_view rest = reply.substr(11);
    if (!IsValidReplId(rest.substr(0, rest.find(' ')))) {
      return Status::IOError("protocol error: invalid replid in +FULLRESYNC");
    }
    spdlog::warn("full resynchronisation from {}:{} is required; it is implemented in M6b", host_, port_);
    return Status::IOError("the master requires a full resynchronisation");
  }
  return Status::IOError("protocol error: unexpected PSYNC reply");
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

Result<ReplyNode> ReplClient::Command(int fd, const std::vector<std::string_view>& args) {
  if (Status st = WriteAll(fd, RespCommand(args), timeout_); !st.ok()) return st;
  auto line = ReadLine(fd);
  if (!line.ok()) return line.status();
  auto node = resp::ParseReply(*line);
  if (!node.ok()) return Status::IOError("protocol error: unreadable reply to " + std::string(args[0]));
  return node;
}

// One reply line, CRLF included. The handshake replies are all single-line (status or error); whatever
// follows the line (the stream after +CONTINUE) stays in in_.
Result<std::string> ReplClient::ReadLine(int fd) {
  const auto deadline = SteadyClock::now() + timeout_;
  for (;;) {
    const size_t end = in_.find("\r\n");
    if (end != std::string::npos) {
      std::string line = in_.substr(0, end + 2);
      in_.erase(0, end + 2);
      return line;
    }
    if (in_.size() > kMaxReplyLine) return Status::IOError("protocol error: reply line too long");
    const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - SteadyClock::now());
    if (left <= 0ms) return Status::TimedOut("no reply from the master within repl-timeout");
    const Status st = ReadSome(fd, &in_, left);
    if (!st.ok() && st.code() != StatusCode::kTimedOut) return st;
  }
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

Status ReplClient::ParseFrames() {
  size_t offset = 0;
  for (;;) {
    size_t consumed = 0;
    const resp::ParseStatus ps = parser_.Parse(std::string_view(in_).substr(offset), &consumed);
    offset += consumed;
    if (ps == resp::ParseStatus::kNeedMore) break;
    if (ps == resp::ParseStatus::kError)
      return Status::IOError("protocol error from master: " + parser_.error());
    if (Status st = HandleFrame(parser_.args()); !st.ok()) return st;
  }
  in_.erase(0, offset);
  if (in_.size() + parser_.buffered_bytes() > kMaxInput) return Status::IOError("master input too large");
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
