#pragma once
#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "common/status.h"
#include "net/connection.h"

namespace tideskv::net {

struct ServerOptions {
  std::string bind = "127.0.0.1";
  uint16_t port = 6666;  // 0 = pick a free port
  int workers = 1;
  // Read at every accept, so runtime CONFIG SET applies to new connections.
  std::function<int()> maxclients = [] { return 10000; };
  std::function<size_t()> query_buffer_limit = [] { return size_t{1} << 30; };
  std::function<void(Session&)> init_session;  // optional
};

struct ServerCounters {
  uint64_t connected = 0;
  uint64_t total_connections = 0;
  uint64_t total_commands = 0;
};

class Server {
 public:
  Server(ServerOptions options, CommandExecutor executor);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  Status Start();  // IOError if the address is in use
  void Stop();     // idempotent; never call from a worker thread
  uint16_t port() const { return port_; }
  ServerCounters counters() const;
  void ResetCounters();

 private:
  struct Worker {
    asio::io_context ctx;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> guard;
    std::thread thread;
  };

  Status Listen();
  void DoAccept();
  void OnAcceptError(const std::error_code& ec);
  void OnAccept(asio::ip::tcp::socket socket, Worker& worker);

  ServerOptions options_;
  CommandExecutor executor_;
  std::shared_ptr<ConnStats> stats_ = std::make_shared<ConnStats>();
  asio::io_context accept_ctx_;
  std::optional<asio::ip::tcp::acceptor> acceptor_;
  std::optional<asio::steady_timer> accept_backoff_;  // accept thread only
  std::chrono::steady_clock::time_point last_accept_warn_{};
  std::thread accept_thread_;
  std::vector<std::unique_ptr<Worker>> workers_;
  size_t next_worker_ = 0;  // only touched on the accept thread
  std::atomic<bool> running_{false};
  uint16_t port_ = 0;
};

}  // namespace tideskv::net
