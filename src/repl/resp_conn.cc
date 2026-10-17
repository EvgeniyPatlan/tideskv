#include "repl/resp_conn.h"

#include "repl/repl_errors.h"
#include "repl/socket_io.h"

namespace tideskv::repl {
namespace {

using namespace std::chrono_literals;
constexpr size_t kMaxReplyLine = size_t{64} * 1024;

}  // namespace

Result<resp::ReplyNode> RespConn::Command(const std::vector<std::string_view>& args) {
  if (Status st = WriteAll(fd_, RespCommand(args), timeout_); !st.ok()) return st;
  auto line = ReadLine();
  if (!line.ok()) return line.status();
  auto node = resp::ParseReply(*line);
  if (!node.ok()) return Status::IOError(std::string(kMsgUnreadableReplyPrefix) + std::string(args[0]));
  return node;
}

Result<std::string> RespConn::ReadLine() {
  const auto deadline = std::chrono::steady_clock::now() + timeout_;
  for (;;) {
    const size_t end = in_->find("\r\n");
    if (end != std::string::npos) {
      std::string line = in_->substr(0, end + 2);
      in_->erase(0, end + 2);
      return line;
    }
    if (in_->size() > kMaxReplyLine) return Status::IOError(std::string(kMsgReplyLineTooLong));
    const auto left =
        std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (left <= 0ms) return Status::TimedOut(std::string(kMsgNoReplyWithinTimeout));
    const Status st = ReadSome(fd_, in_, left);
    if (!st.ok() && st.code() != StatusCode::kTimedOut) return st;
  }
}

}  // namespace tideskv::repl
