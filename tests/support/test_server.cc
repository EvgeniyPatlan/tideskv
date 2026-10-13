#include "support/test_server.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string_view>
#include <thread>

extern char** environ;  // POSIX; declared explicitly rather than relying on _GNU_SOURCE

namespace tideskv::test {

uint16_t TestServer::FreePort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof(addr);
  const bool ok = fd >= 0 && ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
                  ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0;
  if (fd >= 0) ::close(fd);
  if (!ok) {
    ADD_FAILURE() << "could not find a free port";
    return 0;
  }
  return ntohs(addr.sin_port);
}

int TestServer::DecodeStatus(int status) {
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

TestServer::TestServer(std::string binary) : TestServer(std::move(binary), Options{}) {}

namespace {
constexpr int kSpawnAttempts = 5;
constexpr int kExitPortInUse = 3;  // tideskv-server: the port is taken (ServerIT.PortInUseExitsWithCode3)
}  // namespace

TestServer::TestServer(std::string binary, Options options) {
  if (options.dir.empty()) {
    owned_dir_ = std::make_unique<TempDir>("tideskv-it");
    dir_ = owned_dir_->str();
  } else {
    dir_ = options.dir;
  }
  for (int attempt = 1;; ++attempt) {
    port_ = options.port != 0 ? options.port : FreePort();
    Spawn(binary, options);
    if (!options.wait_ready || WaitReady(std::chrono::seconds(10))) return;
    // FreePort() closes its probe socket, so a parallel test may bind the port first (G6). Only then retry.
    const bool lost_port_race = options.port == 0 && !IsRunning() && exit_code_ == kExitPortInUse;
    if (!lost_port_race || attempt == kSpawnAttempts) {
      // A child that already exited is reported here; the destructor must not report it again.
      if (!IsRunning()) exit_consumed_ = true;
      ADD_FAILURE() << "tideskv-server did not become ready; stderr:\n" << Stderr();
      return;
    }
    exit_code_.reset();  // this attempt's exit is handled, not a test failure
  }
}

void TestServer::Spawn(const std::string& binary, const Options& options) {
  std::vector<std::string> args = {binary,      "--port", std::to_string(port_), "--dir", dir_,
                                   "--workers", "2",      "--sync-mode",         "none"};
  args.insert(args.end(), options.extra_args.begin(), options.extra_args.end());
  const std::string log = (log_dir_.path() / "stderr.log").string();

  std::vector<char*> cargs;
  for (auto& a : args) cargs.push_back(a.data());
  cargs.push_back(nullptr);

  // Build the child's environment before fork(): the child may only make async-signal-safe calls.
  std::vector<std::string> env_strings;
  for (char** e = environ; *e != nullptr; ++e) {
    const std::string_view entry(*e);
    const std::string_view name = entry.substr(0, entry.find('='));
    const bool overridden =
        std::any_of(options.env.begin(), options.env.end(), [&](const auto& kv) { return kv.first == name; });
    if (!overridden) env_strings.emplace_back(entry);
  }
  for (const auto& [k, v] : options.env) env_strings.push_back(std::string(k).append("=").append(v));
  std::vector<char*> envp;
  envp.reserve(env_strings.size() + 1);
  for (auto& e : env_strings) envp.push_back(e.data());
  envp.push_back(nullptr);

  pid_ = ::fork();
  if (pid_ == 0) {  // child: async-signal-safe calls only
    const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) ::_exit(126);
    ::dup2(fd, STDOUT_FILENO);
    ::dup2(fd, STDERR_FILENO);
    ::execve(cargs[0], cargs.data(), envp.data());
    ::_exit(127);
  }
  if (pid_ < 0) ADD_FAILURE() << "fork failed";
}

TestServer::~TestServer() {
  if (pid_ > 0) {
    const int code = Terminate();
    if (code < 0) {
      Kill9();
      ADD_FAILURE() << "tideskv-server did not exit after SIGTERM; stderr:\n" << Stderr();
    } else if (code != 0) {
      ADD_FAILURE() << "tideskv-server exited with " << code << "; stderr:\n" << Stderr();
    }
  } else if (exit_code_ && !exit_consumed_ && *exit_code_ != 0) {
    ADD_FAILURE() << "tideskv-server exited unexpectedly with " << *exit_code_ << "; stderr:\n" << Stderr();
  }
}

bool TestServer::WaitReady(std::chrono::milliseconds timeout) {
  // Only our child writes our log, so this line proves *it* owns port_ (G6); a PING alone may reach
  // another test's server on a reused port.
  const std::string ready = "ready to accept connections on port " + std::to_string(port_);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!IsRunning()) return false;
    if (Stderr().find(ready) != std::string::npos) {
      if (auto c = RespClient::Connect(port_, std::chrono::milliseconds(500))) {
        if (c->Send({"PING"}) && c->Read().has_value()) return true;  // PONG or NOAUTH: both mean "up"
      }
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
  pid_ = -1;
  exit_code_ = r > 0 ? DecodeStatus(status) : 255;  // keep the status for Wait()/the destructor
  return false;
}

int TestServer::Wait(std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    if (!IsRunning()) {
      if (!exit_code_) return -1;
      exit_consumed_ = true;
      return *exit_code_;
    }
    if (std::chrono::steady_clock::now() >= deadline) return -1;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
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
  exit_consumed_ = true;
}

std::string TestServer::Stderr() const {
  std::ifstream in(log_dir_.path() / "stderr.log");
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace tideskv::test
