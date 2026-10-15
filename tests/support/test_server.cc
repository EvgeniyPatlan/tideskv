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

// R17: what a sanitizer prints when it finds something. TSan keeps running (halt_on_error=0) and a
// killed child never reaches its exit code, so the log, not the exit status, is what proves a clean run.
constexpr std::string_view kSanitizerMarkers[] = {
    "WARNING: ThreadSanitizer",             // data race, lock-order inversion, signal-unsafe call, ...
    "ERROR: ThreadSanitizer",               // G6: SEGV and other errors TSan reports, then stops
    "FATAL: ThreadSanitizer",               // G6: TSan's runtime giving up (memory mapping, ...)
    "ERROR: AddressSanitizer",              // heap/stack misuse
    "ERROR: LeakSanitizer",                 // leaks at exit (asan build)
    "SUMMARY: UndefinedBehaviorSanitizer",  // asan build: -fsanitize=undefined, -fno-sanitize-recover
    "runtime error:",                       // G6: UBSan's per-site line, also when no SUMMARY follows
    "Sanitizer: CHECK failed",              // a sanitizer runtime aborting on its own limits
};
constexpr size_t kMaxReportBytes = 64 * 1024;

// TSan options the harness adds for every child server, after the inherited (or overridden) ones.
// detect_deadlocks=0 (CF-M6b-11): TSan's deadlock detector CHECK-fails once a thread holds more than 64
// mutexes, and a multi-key command takes one key stripe per distinct key (an MSET of ~100 keys aborts the
// server). That is a tool limit, not a race; data-race detection stays on, and lock-order inversions are
// still checked in-process by unit_tests, which drive the same LockManager, Engine and ReplLog locks.
// suppressions: third-party races judged benign, one by one, in tests/sanitizers/tsan.supp.
std::string WithChildTsanOptions(std::string_view base) {
  std::string out(base);
  if (!out.empty()) out += ':';
  return out.append("detect_deadlocks=0:suppressions=").append(TIDESKV_TSAN_SUPPRESSIONS);
}

// The child's environment: ours, minus the overridden names, plus the overrides; TSAN_OPTIONS always
// carries WithChildTsanOptions (ignored by binaries built without TSan). DEBUGINFOD_URLS is cleared unless
// overridden: Ubuntu sets it, and llvm-symbolizer then downloads debug info for system libraries in the
// middle of a sanitizer report, while the reporting thread holds the server's locks (a TSan child hung
// for minutes holding the gate and ReplLog::mu_).
std::vector<std::string> ChildEnv(const std::vector<std::pair<std::string, std::string>>& overrides) {
  const auto overridden = [&](std::string_view name) {
    return std::any_of(overrides.begin(), overrides.end(), [&](const auto& kv) { return kv.first == name; });
  };
  std::vector<std::string> env;
  std::string tsan;
  for (char** e = environ; *e != nullptr; ++e) {
    const std::string_view entry(*e);
    const size_t eq = entry.find('=');
    const std::string_view name = entry.substr(0, eq);
    if (name == "TSAN_OPTIONS" && eq != std::string_view::npos) tsan = entry.substr(eq + 1);
    if (!overridden(name) && name != "TSAN_OPTIONS" && name != "DEBUGINFOD_URLS") env.emplace_back(entry);
  }
  for (const auto& [k, v] : overrides) {
    if (k == "TSAN_OPTIONS") {
      tsan = v;
    } else {
      env.push_back(std::string(k).append("=").append(v));
    }
  }
  env.push_back("TSAN_OPTIONS=" + WithChildTsanOptions(tsan));
  if (!overridden("DEBUGINFOD_URLS")) env.emplace_back("DEBUGINFOD_URLS=");
  return env;
}
}  // namespace

TestServer::TestServer(std::string binary, Options options)
    : binary_(std::move(binary)), options_(std::move(options)) {
  if (options_.dir.empty()) {
    owned_dir_ = std::make_unique<TempDir>("tideskv-it");
    dir_ = owned_dir_->str();
  } else {
    dir_ = options_.dir;
  }
  SpawnUntilReady();
}

void TestServer::SpawnUntilReady() {
  for (int attempt = 1;; ++attempt) {
    port_ = options_.port != 0 ? options_.port : FreePort();
    log_start_ = Stderr().size();
    Spawn(binary_, options_);
    if (!options_.wait_ready || WaitReady(std::chrono::seconds(10))) return;
    // FreePort() closes its probe socket, so a parallel test may bind the port first (G6); a restart's
    // fixed port may still be held for a moment by the killed predecessor. Only then retry.
    const bool port_busy = !IsRunning() && exit_code_ == kExitPortInUse;
    if (!port_busy || attempt == kSpawnAttempts) {
      // A child that already exited is reported here; the destructor must not report it again.
      if (!IsRunning()) exit_consumed_ = true;
      ADD_FAILURE() << "tideskv-server did not become ready; stderr:\n" << StderrSinceStart();
      return;
    }
    exit_code_.reset();  // this attempt's exit is handled, not a test failure
    if (options_.port != 0) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}

void TestServer::Restart() {
  if (IsRunning() && Terminate() != 0) ADD_FAILURE() << "tideskv-server did not stop cleanly before Restart";
  options_.port = port_;  // the same port: replicas and clients find it again
  exit_code_.reset();
  exit_consumed_ = false;
  paused_ = false;
  SpawnUntilReady();
}

void TestServer::Restart(std::vector<std::string> extra_args) {
  options_.extra_args = std::move(extra_args);
  Restart();
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
  std::vector<std::string> env_strings = ChildEnv(options.env);
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
  // Every run's log (Restart included), however the child ended: SIGTERM, kill -9, or its own exit.
  if (const std::string report = SanitizerReport(); !report.empty()) {
    ADD_FAILURE() << "sanitizer report in tideskv-server (port " << port_ << ", dir " << dir_ << "):\n"
                  << report;
  }
}

bool TestServer::WaitReady(std::chrono::milliseconds timeout) {
  // Only our child writes our log, so this line proves *it* owns port_ (G6); a PING alone may reach
  // another test's server on a reused port. A restart looks only at its own run's lines.
  const std::string ready = "ready to accept connections on port " + std::to_string(port_);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!IsRunning()) return false;
    if (StderrSinceStart().find(ready) != std::string::npos) {
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

void TestServer::Pause() {
  if (pid_ <= 0 || paused_) return;
  ::kill(pid_, SIGSTOP);
  paused_ = true;
}

void TestServer::Resume() {
  if (pid_ <= 0 || !paused_) return;
  ::kill(pid_, SIGCONT);
  paused_ = false;
}

int TestServer::Terminate() {
  if (pid_ <= 0) return -1;
  Resume();  // a stopped process would not act on SIGTERM
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

std::string TestServer::StderrSinceStart() const {
  const std::string all = Stderr();
  return log_start_ <= all.size() ? all.substr(log_start_) : all;
}

std::string TestServer::SanitizerReport() const {
  const std::string log = Stderr();
  size_t first = std::string::npos;
  for (std::string_view marker : kSanitizerMarkers) first = std::min(first, log.find(marker));
  if (first == std::string::npos) return "";
  const size_t line = log.rfind('\n', first);
  const size_t begin = line == std::string::npos ? 0 : line + 1;
  return log.substr(begin, kMaxReportBytes);
}

}  // namespace tideskv::test
