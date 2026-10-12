#include "support/resp_client.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

namespace tideskv::test {

std::string EncodeCommand(const std::vector<std::string>& argv) {
  std::string out = "*" + std::to_string(argv.size()) + "\r\n";
  for (const std::string& a : argv) out += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
  return out;
}

std::string ToDebugString(const RespValue& v) {
  switch (v.type) {
    case RespValue::Type::kNull:
      return "(nil)";
    case RespValue::Type::kInteger:
    case RespValue::Type::kBoolean:
      return "(integer) " + std::to_string(v.integer);
    case RespValue::Type::kError:
      return "(error) " + v.str;
    case RespValue::Type::kArray:
    case RespValue::Type::kMap:
    case RespValue::Type::kSet: {
      std::string out = "[";
      for (const RespValue& e : v.elements) out += ToDebugString(e) + ", ";
      return out + "]";
    }
    default:
      return "\"" + v.str + "\"";
  }
}

std::unique_ptr<RespClient> RespClient::Connect(uint16_t port, std::chrono::milliseconds timeout) {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return nullptr;
  timeval tv{static_cast<time_t>(timeout.count() / 1000),
             static_cast<suseconds_t>((timeout.count() % 1000) * 1000)};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return nullptr;
  }
  return std::unique_ptr<RespClient>(new RespClient(fd));
}

RespClient::~RespClient() { Close(); }

void RespClient::Close() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

bool RespClient::SendRaw(std::string_view bytes) {
  while (!bytes.empty()) {
    const ssize_t n = ::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    if (n <= 0) return false;
    bytes.remove_prefix(static_cast<size_t>(n));
  }
  return true;
}

bool RespClient::Fill() {
  if (pos_ > 65536) {
    buf_.erase(0, pos_);
    pos_ = 0;
  }
  char tmp[65536];
  const ssize_t n = ::recv(fd_, tmp, sizeof(tmp), 0);
  if (n <= 0) return false;
  buf_.append(tmp, static_cast<size_t>(n));
  return true;
}

std::optional<std::string> RespClient::ReadLine() {
  for (;;) {
    const size_t eol = buf_.find("\r\n", pos_);
    if (eol != std::string::npos) {
      std::string line = buf_.substr(pos_, eol - pos_);
      pos_ = eol + 2;
      return line;
    }
    if (!Fill()) return std::nullopt;
  }
}

std::optional<std::string> RespClient::ReadExact(size_t n) {
  while (buf_.size() - pos_ < n + 2) {
    if (!Fill()) return std::nullopt;
  }
  std::string data = buf_.substr(pos_, n);
  pos_ += n + 2;  // skip trailing CRLF
  return data;
}

std::optional<RespValue> RespClient::ReadAggregate(RespValue::Type type, int64_t count) {
  RespValue v;
  v.type = type;
  if (count < 0) {
    v.type = RespValue::Type::kNull;
    return v;
  }
  const int64_t items = type == RespValue::Type::kMap ? count * 2 : count;
  for (int64_t i = 0; i < items; ++i) {
    auto e = ReadValue();
    if (!e) return std::nullopt;
    v.elements.push_back(std::move(*e));
  }
  return v;
}

std::optional<RespValue> RespClient::ReadValue() {
  auto line = ReadLine();
  if (!line || line->empty()) return std::nullopt;
  const char tag = (*line)[0];
  const std::string rest = line->substr(1);
  RespValue v;
  switch (tag) {
    case '+':
      v.type = RespValue::Type::kSimple;
      v.str = rest;
      return v;
    case '-':
      v.type = RespValue::Type::kError;
      v.str = rest;
      return v;
    case ':':
      v.type = RespValue::Type::kInteger;
      v.integer = std::stoll(rest);
      return v;
    case ',':
      v.type = RespValue::Type::kDouble;
      v.str = rest;
      return v;
    case '#':
      v.type = RespValue::Type::kBoolean;
      v.integer = rest == "t" ? 1 : 0;
      return v;
    case '_':
      v.type = RespValue::Type::kNull;
      return v;
    case '*':
      return ReadAggregate(RespValue::Type::kArray, std::stoll(rest));
    case '%':
      return ReadAggregate(RespValue::Type::kMap, std::stoll(rest));
    case '~':
      return ReadAggregate(RespValue::Type::kSet, std::stoll(rest));
    case '$':
    case '=': {
      const int64_t len = std::stoll(rest);
      if (len < 0) return v;  // null bulk
      auto data = ReadExact(static_cast<size_t>(len));
      if (!data) return std::nullopt;
      v.type = tag == '$' ? RespValue::Type::kBulk : RespValue::Type::kVerbatim;
      v.str = tag == '$' ? *data : data->substr(4);  // drop "txt:"
      return v;
    }
    default:
      return std::nullopt;
  }
}

std::optional<RespValue> RespClient::Read() { return fd_ < 0 ? std::nullopt : ReadValue(); }

RespValue RespClient::Command(const std::vector<std::string>& argv) {
  if (Send(argv)) {
    if (auto reply = Read()) return *reply;
  }
  ADD_FAILURE() << "no reply to " << (argv.empty() ? "" : argv[0]);
  RespValue err;
  err.type = RespValue::Type::kError;
  err.str = "<no reply>";
  return err;
}

bool RespClient::WaitForClose(std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd pfd{fd_, POLLIN, 0};
    if (::poll(&pfd, 1, 50) <= 0) continue;
    char tmp[4096];
    const ssize_t n = ::recv(fd_, tmp, sizeof(tmp), 0);
    if (n == 0 || (n < 0 && errno == ECONNRESET)) return true;
  }
  return false;
}

}  // namespace tideskv::test
