#include "net/server.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>
#include <utility>

#include "types/errors.h"

namespace tideskv::net {

Server::Server(ServerOptions options, CommandExecutor executor)
    : options_(std::move(options)), executor_(std::move(executor)) {}

Server::~Server() { Stop(); }

namespace {
constexpr auto kAcceptBackoff = std::chrono::milliseconds(50);
constexpr auto kAcceptWarnInterval = std::chrono::seconds(1);
}  // namespace

Status Server::Listen() {
  std::error_code ec;
  const asio::ip::address addr = asio::ip::make_address(options_.bind, ec);
  if (ec) return Status::InvalidArgument("invalid bind address '" + options_.bind + "'");
  const asio::ip::tcp::endpoint endpoint(addr, options_.port);
  acceptor_.emplace(accept_ctx_);
  const auto fail = [&](const char* step) {
    acceptor_.reset();
    return Status::IOError(std::string(step) + " " + options_.bind + ":" + std::to_string(options_.port) +
                           ": " + ec.message());
  };
  // With ASIO_NO_DEPRECATED these error_code overloads return void: check ec after each.
  acceptor_->open(endpoint.protocol(), ec);
  if (ec) return fail("open");
  acceptor_->set_option(asio::socket_base::reuse_address(true), ec);
  if (ec) return fail("setsockopt");
  acceptor_->bind(endpoint, ec);
  if (ec) return fail("bind");
  acceptor_->listen(asio::socket_base::max_listen_connections, ec);
  if (ec) return fail("listen");
  const auto local = acceptor_->local_endpoint(ec);
  if (ec) return fail("local_endpoint");
  port_ = local.port();
  return Status::Ok();
}

Status Server::Start() {
  if (running_ || acceptor_) return Status::InvalidArgument("server already started");
  if (Status st = Listen(); !st.ok()) return st;
  const int n = std::max(1, options_.workers);
  for (int i = 0; i < n; ++i) {
    auto w = std::make_unique<Worker>();
    w->guard.emplace(asio::make_work_guard(w->ctx));
    w->thread = std::thread([ctx = &w->ctx] { ctx->run(); });
    workers_.push_back(std::move(w));
  }
  accept_backoff_.emplace(accept_ctx_);
  running_ = true;
  DoAccept();
  accept_thread_ = std::thread([this] { accept_ctx_.run(); });
  return Status::Ok();
}

void Server::DoAccept() {
  if (!acceptor_) {  // engaged by Listen(); reset only by Stop() after the accept thread exits
    spdlog::error("internal error: DoAccept() without an acceptor");
    return;
  }
  Worker& worker = *workers_[next_worker_++ % workers_.size()];
  acceptor_->async_accept(worker.ctx, [this, &worker](std::error_code ec, asio::ip::tcp::socket socket) {
    if (!running_) return;
    if (ec == asio::error::operation_aborted) return;
    if (ec == asio::error::connection_aborted) return DoAccept();
    if (ec) return OnAcceptError(ec);
    OnAccept(std::move(socket), worker);
    DoAccept();
  });
}

// Persistent failures (EMFILE/ENFILE) would otherwise spin: back off before re-arming.
void Server::OnAcceptError(const std::error_code& ec) {
  const auto now = std::chrono::steady_clock::now();
  if (now - last_accept_warn_ >= kAcceptWarnInterval) {
    last_accept_warn_ = now;
    spdlog::warn("accept failed: {}", ec.message());
  }
  if (!accept_backoff_) {  // engaged by Start() before the first DoAccept()
    spdlog::error("internal error: accept backoff timer missing; accepting stopped");
    return;
  }
  accept_backoff_->expires_after(kAcceptBackoff);
  accept_backoff_->async_wait([this](std::error_code wait_ec) {
    if (!wait_ec && running_) DoAccept();
  });
}

void Server::OnAccept(asio::ip::tcp::socket socket, Worker& worker) {
  std::error_code ec;
  if (stats_->connected.load() >= static_cast<uint64_t>(options_.maxclients())) {
    const std::string reply = "-" + std::string(kErrMaxClients) + "\r\n";
    asio::write(socket, asio::buffer(reply), ec);
    socket.close(ec);
    return;
  }
  socket.set_option(asio::ip::tcp::no_delay(true), ec);
  Session session;
  session.id = stats_->next_id.fetch_add(1);
  const auto remote = socket.remote_endpoint(ec);
  session.peer = ec ? "?" : remote.address().to_string() + ":" + std::to_string(remote.port());
  if (options_.init_session) options_.init_session(session);
  auto conn = std::make_shared<Connection>(std::move(socket), std::move(session), executor_, stats_,
                                           options_.query_buffer_limit(), options_.on_disconnect);
  asio::post(worker.ctx, [conn] { conn->Start(); });  // start on the connection's own thread
}

void Server::Stop() {
  if (!running_.exchange(false)) return;
  asio::post(accept_ctx_, [this] {
    std::error_code ignored;
    acceptor_->close(ignored);  // the pending accept completes with operation_aborted
    accept_backoff_->cancel();
  });
  if (accept_thread_.joinable()) accept_thread_.join();
  for (auto& w : workers_) {
    w->guard.reset();
    w->ctx.stop();
  }
  for (auto& w : workers_) {
    if (w->thread.joinable()) w->thread.join();
  }
  workers_.clear();  // destroys pending handlers → last shared_ptr<Connection> → sockets close
  acceptor_.reset();
}

ServerCounters Server::counters() const {
  return {stats_->connected.load(), stats_->total_connections.load(), stats_->total_commands.load()};
}

void Server::ResetCounters() {
  stats_->total_connections.store(0);
  stats_->total_commands.store(0);
}

}  // namespace tideskv::net
