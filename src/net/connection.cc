#include "net/connection.h"

#include <spdlog/spdlog.h>

#include <string_view>
#include <utility>

#include "types/errors.h"

namespace tideskv::net {

Connection::Connection(asio::ip::tcp::socket socket, Session session, CommandExecutor executor,
                       std::shared_ptr<ConnStats> stats, size_t query_buffer_limit)
    : socket_(std::move(socket)),
      block_timer_(socket_.get_executor()),
      session_(std::move(session)),
      executor_(std::move(executor)),
      stats_(std::move(stats)),
      query_buffer_limit_(query_buffer_limit),
      reply_(session_.protocol) {
  stats_->connected.fetch_add(1);
  stats_->total_connections.fetch_add(1);
}

Connection::~Connection() {
  if (session_.blocked) session_.blocked->Cancel();  // e.g. Server::Stop destroying pending handlers
  stats_->connected.fetch_sub(1);
}

void Connection::Start() {
  session_.link = std::static_pointer_cast<ClientLink>(shared_from_this());
  DoRead();
}

void Connection::DoRead() {
  if (closed_ || reading_) return;
  reading_ = true;
  socket_.async_read_some(asio::buffer(read_buf_),
                          [self = shared_from_this()](std::error_code ec, size_t n) { self->OnRead(ec, n); });
}

void Connection::OnRead(std::error_code ec, size_t n) {
  reading_ = false;
  if (ec) return Close();  // EOF, reset, or shutdown
  in_.append(read_buf_.data(), n);
  if (in_.size() + parser_.buffered_bytes() > query_buffer_limit_) {
    spdlog::warn("closing client id={} addr={}: query buffer limit exceeded", session_.id, session_.peer);
    return Close();
  }
  if (!session_.blocked) ProcessInput();  // while blocked, input is only buffered
  AfterInput();
}

void Connection::ProcessInput() {
  size_t offset = 0;
  while (!session_.close_after_reply) {
    size_t consumed = 0;
    parser_.SetUnauthenticated(!session_.authenticated);
    const resp::ParseStatus st = parser_.Parse(std::string_view(in_).substr(offset), &consumed);
    offset += consumed;
    if (st == resp::ParseStatus::kNeedMore) break;
    if (st == resp::ParseStatus::kError) {
      reply_.Error("ERR " + parser_.error());
      session_.close_after_reply = true;
      break;
    }
    stats_->total_commands.fetch_add(1, std::memory_order_relaxed);
    const size_t reply_mark = reply_.size();
    try {
      executor_(session_, parser_.args(), reply_);
    } catch (const std::exception& e) {
      spdlog::error("command failed for client id={}: {}", session_.id, e.what());
      reply_.Truncate(reply_mark);  // drop any half-written reply
      reply_.Error(kErrInternal);
      session_.close_after_reply = true;
    }
    if (session_.blocked) {  // the command parked itself: stop until it ends (F5)
      ArmBlockTimer();
      break;
    }
  }
  in_.erase(0, offset);
}

void Connection::AfterInput() {
  if (closed_) return;
  if (!reply_.empty()) {
    StartWrite();
  } else if (session_.close_after_reply && !writing_) {
    return Close();
  }
  // Blocked: keep reading (EOF detection, buffering). Otherwise read only when no write is in flight.
  if (session_.blocked || !writing_) DoRead();
}

void Connection::StartWrite() {
  if (writing_ || closed_) return;
  writing_ = true;
  out_ = reply_.Take();
  asio::async_write(socket_, asio::buffer(out_),
                    [self = shared_from_this()](std::error_code ec, size_t) { self->OnWrite(ec); });
}

void Connection::OnWrite(std::error_code ec) {
  writing_ = false;
  if (ec) return Close();
  if (!reply_.empty()) return StartWrite();
  if (session_.close_after_reply) return Close();
  DoRead();
}

void Connection::Unblock(std::string reply) {
  asio::post(socket_.get_executor(),
             [self = shared_from_this(), r = std::move(reply)]() { self->OnUnblocked(r); });
}

// Relies on at-most-once delivery: BlockedClient ends a wait exactly once (TryServe, Cancel and Unblock
// all check and set done_ under its lock), so at most one Unblock is posted per wait, and never after
// the block timer's Cancel won. A non-null session_.blocked therefore means this reply belongs to the
// current wait; nothing here has to tell a stale reply from a fresh one.
void Connection::OnUnblocked(const std::string& reply) {
  if (closed_ || !session_.blocked) return;
  reply_.Raw(reply);
  ResumeAfterBlock();
}

void Connection::ResumeAfterBlock() {
  session_.blocked.reset();
  block_timer_.cancel();
  ProcessInput();  // the pipelined commands behind the blocking one (one may block again)
  AfterInput();
}

void Connection::ArmBlockTimer() {
  const auto deadline = session_.blocked->deadline();
  if (!deadline) return;  // timeout 0: wait forever
  block_timer_.expires_at(*deadline);
  block_timer_.async_wait([self = shared_from_this(), blocked = session_.blocked](std::error_code ec) {
    if (ec || self->closed_ || self->session_.blocked != blocked) return;  // cancelled or superseded
    if (!blocked->Cancel()) return;  // served concurrently: that reply is already posted to us
    self->reply_.NullArray();        // Redis replyToBlockedClientTimedOut for list/zset (F5)
    self->ResumeAfterBlock();
  });
}

void Connection::Close() {
  if (closed_) return;
  closed_ = true;
  if (session_.blocked) {
    session_.blocked->Cancel();
    session_.blocked.reset();
  }
  block_timer_.cancel();
  std::error_code ignored;
  socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
  socket_.close(ignored);
}

}  // namespace tideskv::net
