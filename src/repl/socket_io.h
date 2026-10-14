#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"

// Blocking POSIX socket helpers for the replication threads (M6a D10). Nothing here touches asio. Every
// call bounds its wait with poll() and uses MSG_DONTWAIT, so it behaves the same whether the descriptor
// is in blocking or non-blocking mode (a socket released by asio is non-blocking).
namespace tideskv::repl {

// An owning file descriptor (RAII, move-only).
class Fd {
 public:
  explicit Fd(int fd = -1) : fd_(fd) {}
  ~Fd();
  Fd(Fd&& other) noexcept : fd_(other.release()) {}
  Fd& operator=(Fd&& other) noexcept;
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;

  int get() const { return fd_; }
  int release();  // gives up ownership; returns the descriptor (or -1)

 private:
  int fd_;
};

// Resolves host and connects within the timeout. IOError on resolution, refusal or timeout. The returned
// socket is non-blocking and close-on-exec.
Result<Fd> ConnectTcp(const std::string& host, uint16_t port, std::chrono::milliseconds timeout);
// Sends every byte of data (send with MSG_NOSIGNAL). TimedOut if the peer does not drain it in time;
// IOError on a socket error (peer gone, shutdown).
Status WriteAll(int fd, std::string_view data, std::chrono::milliseconds timeout);
// Waits up to timeout (0 = poll once) for input and appends what one recv returns to *out. TimedOut when
// nothing arrived; IOError on EOF or a socket error.
Status ReadSome(int fd, std::string* out, std::chrono::milliseconds timeout);
// "*n\r\n$len\r\narg\r\n..." (binary-safe).
std::string RespCommand(const std::vector<std::string_view>& args);

}  // namespace tideskv::repl
