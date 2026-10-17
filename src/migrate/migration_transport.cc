#include "migrate/migration_transport.h"

#include <sys/socket.h>

#include <algorithm>
#include <utility>

#include "repl/repl_errors.h"
#include "repl/resp_conn.h"
#include "repl/socket_io.h"

namespace tideskv::migrate {
namespace {

using namespace std::chrono_literals;

// The target answers CLUSTER IMPORT <R> 0 only once its migrate worker ran the cleanup of an overlapping
// range, which it waits for up to ~20 s (Task 9). The start request may take that long without the job
// failing, so its reply timeout is at least this, whatever repl-timeout says.
constexpr std::chrono::milliseconds kImportStartTimeout = 30s;

bool IsImportStart(const std::vector<std::string_view>& args) {
  return args.size() == 4 && args[0] == "CLUSTER" && args[1] == "IMPORT" && args[3] == "0";
}

class RespTransport final : public MigrationTransport {
 public:
  RespTransport(repl::Fd fd, std::chrono::milliseconds timeout)
      : fd_(std::move(fd)), timeout_(timeout), conn_(fd_.get(), &in_, timeout) {}

  Status Call(const std::vector<std::string_view>& args) override {
    const bool slow = IsImportStart(args);
    if (slow) conn_.set_timeout(std::max(timeout_, kImportStartTimeout));
    auto reply = conn_.Command(args);
    if (slow) conn_.set_timeout(timeout_);
    if (!reply.ok()) return reply.status();
    if (reply->type == resp::ReplyNode::Type::kError) return Status::UserError(reply->str);
    if (reply->type == resp::ReplyNode::Type::kStatus) return Status::Ok();
    return Status::IOError(std::string(kMsgUnexpectedReplyPrefix) + std::string(args[0]));
  }
  void Shutdown() override { ::shutdown(fd_.get(), SHUT_RDWR); }  // wakes a poll in WriteAll/ReadSome

 private:
  repl::Fd fd_;
  std::chrono::milliseconds timeout_;
  std::string in_;
  repl::RespConn conn_;  // refers to fd_ and in_
};

}  // namespace

Result<std::unique_ptr<MigrationTransport>> ConnectResp(const std::string& host, uint16_t port,
                                                        std::chrono::milliseconds timeout,
                                                        const std::atomic<bool>* cancel) {
  auto fd = repl::ConnectTcp(host, port, timeout, cancel);
  if (!fd.ok()) return fd.status();
  return std::unique_ptr<MigrationTransport>(std::make_unique<RespTransport>(std::move(fd).value(), timeout));
}

}  // namespace tideskv::migrate
