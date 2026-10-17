#pragma once
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "resp/reply_parser.h"

// A blocking RESP request / single-line reply over a socket someone else owns (M6a D8; M7b D3): the replica's
// handshake and the slot migrator's session. Bytes read past a reply line stay in *in for the owner (the
// replication stream that follows +CONTINUE).
namespace tideskv::repl {

class RespConn {
 public:
  RespConn(int fd, std::string* in, std::chrono::milliseconds timeout)
      : fd_(fd), in_(in), timeout_(timeout) {}

  // Writes the command, then reads one reply line; each within the timeout. TimedOut or IOError on I/O;
  // IOError "protocol error: unreadable reply to <cmd>" for a line resp::ParseReply rejects. An error reply
  // (-ERR ...) is a value, not a failure.
  Result<resp::ReplyNode> Command(const std::vector<std::string_view>& args);
  Result<std::string> ReadLine();  // one reply line, CRLF included
  void set_timeout(std::chrono::milliseconds t) { timeout_ = t; }

 private:
  int fd_;
  std::string* in_;
  std::chrono::milliseconds timeout_;
};

}  // namespace tideskv::repl
