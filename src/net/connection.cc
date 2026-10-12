#include "net/connection.h"

#include <spdlog/spdlog.h>

#include <string_view>
#include <utility>

namespace tideskv::net {

Connection::Connection(asio::ip::tcp::socket socket, Session session, CommandExecutor executor,
                       std::shared_ptr<ConnStats> stats, size_t query_buffer_limit)
    : socket_(std::move(socket)),
      session_(std::move(session)),
      executor_(std::move(executor)),
      stats_(std::move(stats)),
      query_buffer_limit_(query_buffer_limit),
      reply_(session_.protocol) {
  stats_->connected.fetch_add(1);
  stats_->total_connections.fetch_add(1);
}

Connection::~Connection() { stats_->connected.fetch_sub(1); }

void Connection::Start() { DoRead(); }

void Connection::DoRead() {
  socket_.async_read_some(asio::buffer(read_buf_),
                          [self = shared_from_this()](std::error_code ec, size_t n) { self->OnRead(ec, n); });
}

void Connection::OnRead(std::error_code ec, size_t n) {
  if (ec) return Close();  // EOF, reset, or shutdown
  in_.append(read_buf_.data(), n);
  if (in_.size() + parser_.buffered_bytes() > query_buffer_limit_) {
    spdlog::warn("closing client id={} addr={}: query buffer limit exceeded", session_.id, session_.peer);
    return Close();
  }
  ProcessInput();
  if (!reply_.empty()) return Flush();
  if (session_.close_after_reply) return Close();
  DoRead();
}

void Connection::ProcessInput() {
  size_t offset = 0;
  while (!session_.close_after_reply) {
    size_t consumed = 0;
    const resp::ParseStatus st = parser_.Parse(std::string_view(in_).substr(offset), &consumed);
    offset += consumed;
    if (st == resp::ParseStatus::kNeedMore) break;
    if (st == resp::ParseStatus::kError) {
      reply_.Error("ERR " + parser_.error());
      session_.close_after_reply = true;
      break;
    }
    stats_->total_commands.fetch_add(1, std::memory_order_relaxed);
    executor_(session_, parser_.args(), reply_);
  }
  in_.erase(0, offset);
}

void Connection::Flush() {
  out_ = reply_.Take();
  asio::async_write(socket_, asio::buffer(out_), [self = shared_from_this()](std::error_code ec, size_t) {
    if (ec || self->session_.close_after_reply) return self->Close();
    self->DoRead();
  });
}

void Connection::Close() {
  if (closed_) return;
  closed_ = true;
  std::error_code ignored;
  socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
  socket_.close(ignored);
}

}  // namespace tideskv::net
