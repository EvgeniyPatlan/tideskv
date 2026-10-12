#include "support/test_server.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fstream>
#include <sstream>
#include <thread>

namespace tideskv::test {

uint16_t TestServer::FreePort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  socklen_t len = sizeof(addr);
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  ::close(fd);
  return ntohs(addr.sin_port);
}

TestServer::TestServer(std::string binary) : TestServer(std::move(binary), Options{}) {}

TestServer::TestServer(std::string binary, Options options) {
  if (options.dir.empty()) {
    owned_dir_ = std::make_unique<TempDir>("tideskv-it");
    dir_ = owned_dir_->str();
  } else {
    dir_ = options.dir;
  }
  port_ = options.port != 0 ? options.port : FreePort();
  std::vector<std::string> args = {binary,      "--port", std::to_string(port_), "--dir", dir_,
                                   "--workers", "2",      "--sync-mode",         "none"};
  args.insert(args.end(), options.extra_args.begin(), options.extra_args.end());
  const std::string log = (log_dir_.path() / "stderr.log").string();

  pid_ = ::fork();
  if (pid_ == 0) {  // child
    const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    ::dup2(fd, STDOUT_FILENO);
    ::dup2(fd, STDERR_FILENO);
    std::vector<char*> cargs;
    for (auto& a : args) cargs.push_back(a.data());
    cargs.push_back(nullptr);
    ::execv(binary.c_str(), cargs.data());
    ::_exit(127);
  }
  if (options.wait_ready && !WaitReady(std::chrono::seconds(10))) {
    ADD_FAILURE() << "tideskv-server did not become ready; stderr:\n" << Stderr();
  }
}

TestServer::~TestServer() {
  if (pid_ <= 0) return;
  if (Terminate() < 0) Kill9();
}

bool TestServer::WaitReady(std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!IsRunning()) return false;
    if (auto c = RespClient::Connect(port_, std::chrono::milliseconds(500))) {
      if (c->Send({"PING"}) && c->Read().has_value()) return true;  // PONG or NOAUTH: both mean "up"
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

std::unique_ptr<RespClient> TestServer::Connect() const { return RespClient::Connect(port_); }

bool TestServer::IsRunning() {
  if (pid_ <= 0) return false;
  int status = 0;
  const pid_t r = ::waitpid(pid_, &status, WNOHANG);
  if (r == 0) return true;
  pid_ = -1;  // reaped (status lost; use Wait() when the code matters)
  return false;
}

int TestServer::Wait(std::chrono::milliseconds timeout) {
  if (pid_ <= 0) return -1;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    int status = 0;
    const pid_t r = ::waitpid(pid_, &status, WNOHANG);
    if (r == pid_) {
      pid_ = -1;
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return -1;
}

int TestServer::Terminate() {
  if (pid_ <= 0) return -1;
  ::kill(pid_, SIGTERM);
  return Wait(std::chrono::seconds(10));
}

void TestServer::Kill9() {
  if (pid_ <= 0) return;
  ::kill(pid_, SIGKILL);
  int status = 0;
  ::waitpid(pid_, &status, 0);
  pid_ = -1;
}

std::string TestServer::Stderr() const {
  std::ifstream in(log_dir_.path() / "stderr.log");
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace tideskv::test
