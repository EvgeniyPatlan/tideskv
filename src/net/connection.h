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
#include "server/blocking.h"
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
// shared_ptr captured in its pending async handler. While a command is blocked (session_.blocked set) it
// executes nothing more but keeps reading, so pipelined input is buffered and EOF is detected (M4 F5).
class Connection : public std::enable_shared_from_this<Connection>, public ClientLink {
 public:
  Connection(asio::ip::tcp::socket socket, Session session, CommandExecutor executor,
             std::shared_ptr<ConnStats> stats, size_t query_buffer_limit,
             std::function<void(Session&)> on_disconnect = nullptr);
  ~Connection() override;
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  void Start();
  void Unblock(std::string reply) override;          // ClientLink: any thread
  void UnblockAndClose(std::string reply) override;  // ClientLink: any thread

 private:
  void DoRead();
  void OnRead(std::error_code ec, size_t n);
  void ProcessInput();
  void AfterInput();
  void StartWrite();
  void OnWrite(std::error_code ec);
  void ArmBlockTimer();
  void OnUnblocked(const std::string& reply, bool close_after);
  void ResumeAfterBlock();  // the blocked command ended: run the pipeline again
  void Handoff();           // PSYNC's reply is flushed: give the socket to session_.takeover (M6a D10)
  void FinishHandoff();
  void Close();

  asio::ip::tcp::socket socket_;
  asio::steady_timer block_timer_;
  Session session_;
  CommandExecutor executor_;
  std::shared_ptr<ConnStats> stats_;
  size_t query_buffer_limit_;
  std::function<void(Session&)> on_disconnect_;
  resp::RequestParser parser_;
  resp::ReplyBuilder reply_;
  std::string in_;
  std::string out_;
  std::array<char, size_t{16} * 1024> read_buf_{};
  bool closed_ = false;
  bool counted_ = true;           // in stats_->connected; false once the socket was handed off
  bool reading_ = false;          // at most one read in flight
  bool writing_ = false;          // at most one write in flight
  bool handoff_pending_ = false;  // Handoff() cancelled an in-flight read and waits for its completion
};

}  // namespace tideskv::net
