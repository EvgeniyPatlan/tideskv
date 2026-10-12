#pragma once
#include <array>
#include <asio.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "resp/reply_builder.h"
#include "resp/request_parser.h"
#include "server/session.h"

namespace tideskv::net {

using CommandExecutor = std::function<void(Session&, const std::vector<std::string>&, resp::ReplyBuilder&)>;

struct ConnStats {
  std::atomic<uint64_t> connected{0};
  std::atomic<uint64_t> total_connections{0};
  std::atomic<uint64_t> total_commands{0};
  std::atomic<uint64_t> next_id{1};
};

// One client. Lives on exactly one worker io_context; kept alive by the
// shared_ptr captured in its pending async handler.
class Connection : public std::enable_shared_from_this<Connection> {
 public:
  Connection(asio::ip::tcp::socket socket, Session session, CommandExecutor executor,
             std::shared_ptr<ConnStats> stats, size_t query_buffer_limit);
  ~Connection();
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  void Start();

 private:
  void DoRead();
  void OnRead(std::error_code ec, size_t n);
  void ProcessInput();
  void Flush();
  void Close();

  asio::ip::tcp::socket socket_;
  Session session_;
  CommandExecutor executor_;
  std::shared_ptr<ConnStats> stats_;
  size_t query_buffer_limit_;
  resp::RequestParser parser_;
  resp::ReplyBuilder reply_;
  std::string in_;
  std::string out_;
  std::array<char, size_t{16} * 1024> read_buf_{};
  bool closed_ = false;
};

}  // namespace tideskv::net
