#include "repl/socket_io.h"

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <memory>

namespace tideskv::repl {
namespace {

using Clock = std::chrono::steady_clock;

std::string Errno(std::string_view what) { return std::string(what) + ": " + std::strerror(errno); }

// Milliseconds left until deadline, rounded up so a sub-millisecond remainder still waits.
int RemainingMs(Clock::time_point deadline) {
  const auto left = deadline - Clock::now();
  if (left <= Clock::duration::zero()) return 0;
  return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(left).count());
}

// poll() for `events` until the deadline: >0 ready, 0 timed out, <0 error (errno set). Retries EINTR.
int PollUntil(int fd, int16_t events, Clock::time_point deadline) {
  for (;;) {
    pollfd p{fd, events, 0};
    const int n = ::poll(&p, 1, RemainingMs(deadline));
    if (n >= 0 || errno != EINTR) return n;
  }
}

// PollUntil in slices of kCancelSlice, giving up (0, as a timeout) once *cancel is set.
constexpr auto kCancelSlice = std::chrono::milliseconds(50);
int PollCancellable(int fd, int16_t events, Clock::time_point deadline, const std::atomic<bool>* cancel) {
  if (cancel == nullptr) return PollUntil(fd, events, deadline);
  for (;;) {
    if (cancel->load(std::memory_order_acquire)) return 0;
    const auto slice_end = std::min(deadline, Clock::now() + kCancelSlice);
    const int n = PollUntil(fd, events, slice_end);
    if (n != 0 || slice_end >= deadline) return n;
  }
}

// One non-blocking connect attempt to `ai`, finished with poll within the deadline.
Result<Fd> ConnectOne(const addrinfo& ai, Clock::time_point deadline, const std::atomic<bool>* cancel) {
  Fd fd(::socket(ai.ai_family, ai.ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, ai.ai_protocol));
  if (fd.get() < 0) return Status::IOError(Errno("socket"));
  if (::connect(fd.get(), ai.ai_addr, ai.ai_addrlen) == 0) return fd;
  if (errno != EINPROGRESS) return Status::IOError(Errno("connect"));
  const int ready = PollCancellable(fd.get(), POLLOUT, deadline, cancel);
  if (ready == 0 && cancel != nullptr && cancel->load()) return Status::IOError("connect: cancelled");
  if (ready == 0) return Status::IOError("connect: timed out");
  if (ready < 0) return Status::IOError(Errno("poll"));
  int err = 0;
  socklen_t len = sizeof(err);
  if (::getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &err, &len) != 0)
    return Status::IOError(Errno("getsockopt"));
  if (err != 0) return Status::IOError(std::string("connect: ") + std::strerror(err));
  return fd;
}

}  // namespace

Fd::~Fd() {
  if (fd_ >= 0) ::close(fd_);
}

Fd& Fd::operator=(Fd&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = other.release();
  }
  return *this;
}

int Fd::release() {
  const int fd = fd_;
  fd_ = -1;
  return fd;
}

Result<Fd> ConnectTcp(const std::string& host, uint16_t port, std::chrono::milliseconds timeout,
                      const std::atomic<bool>* cancel) {
  const auto deadline = Clock::now() + timeout;
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* found = nullptr;
  const int rc = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found);
  if (rc != 0) return Status::IOError("cannot resolve " + host + ": " + ::gai_strerror(rc));
  const std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> list(found, &::freeaddrinfo);
  Status last = Status::IOError("no address for " + host);
  for (const addrinfo* ai = list.get(); ai != nullptr; ai = ai->ai_next) {
    auto fd = ConnectOne(*ai, deadline, cancel);
    if (fd.ok()) return fd;
    last = fd.status();
  }
  return last;
}

Status WriteAll(int fd, std::string_view data, std::chrono::milliseconds timeout) {
  const auto deadline = Clock::now() + timeout;
  while (!data.empty()) {
    const ssize_t n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0) {
      data.remove_prefix(static_cast<size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return Status::IOError(Errno("send"));
    const int ready = PollUntil(fd, POLLOUT, deadline);
    if (ready == 0) return Status::TimedOut("send");
    if (ready < 0) return Status::IOError(Errno("poll"));
  }
  return Status::Ok();
}

Status ReadSome(int fd, std::string* out, std::chrono::milliseconds timeout) {
  const int ready = PollUntil(fd, POLLIN, Clock::now() + timeout);
  if (ready == 0) return Status::TimedOut("recv");
  if (ready < 0) return Status::IOError(Errno("poll"));
  std::array<char, size_t{16} * 1024> buf{};
  for (;;) {
    const ssize_t n = ::recv(fd, buf.data(), buf.size(), MSG_DONTWAIT);
    if (n > 0) {
      out->append(buf.data(), static_cast<size_t>(n));
      return Status::Ok();
    }
    if (n == 0) return Status::IOError("connection closed by peer");
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return Status::TimedOut("recv");  // spurious wakeup
    return Status::IOError(Errno("recv"));
  }
}

std::string RespCommand(const std::vector<std::string_view>& args) {
  std::string out = "*" + std::to_string(args.size()) + "\r\n";
  for (std::string_view a : args) {
    out.append("$").append(std::to_string(a.size())).append("\r\n").append(a).append("\r\n");
  }
  return out;
}

}  // namespace tideskv::repl
