#pragma once
#include <sys/types.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "support/resp_client.h"
#include "support/temp_dir.h"

namespace tideskv::test {

// Runs the real tideskv-server binary as a child process.
class TestServer {
 public:
  struct Options {
    // appended last, so they override defaults; must not redirect or silence the log: readiness reads
    // "ready to accept connections" from it
    std::vector<std::string> extra_args;
    std::string dir;                                       // data dir; empty = a private temp dir
    uint16_t port = 0;                                     // 0 = a free port
    bool wait_ready = true;                                // wait until the server answers
    std::vector<std::pair<std::string, std::string>> env;  // overrides the child's environment
  };

  // Two overloads rather than a defaulted Options: GCC cannot default-construct a
  // nested aggregate with member initializers inside its enclosing class.
  explicit TestServer(std::string binary);
  TestServer(std::string binary, Options options);
  ~TestServer();
  TestServer(const TestServer&) = delete;
  TestServer& operator=(const TestServer&) = delete;

  uint16_t port() const { return port_; }
  const std::string& dir() const { return dir_; }
  std::unique_ptr<RespClient> Connect() const;
  // Exit code (0..255), 128+signal if killed, or -1 on timeout.
  int Wait(std::chrono::milliseconds timeout = std::chrono::seconds(10));
  int Terminate();  // SIGTERM, then Wait()
  void Kill9();
  bool IsRunning();
  std::string Stderr() const;

 private:
  static int DecodeStatus(int status);
  void Spawn(const std::string& binary, const Options& options);
  bool WaitReady(std::chrono::milliseconds timeout);
  static uint16_t FreePort();

  TempDir log_dir_{"tideskv-it-log"};
  std::unique_ptr<TempDir> owned_dir_;
  std::string dir_;
  uint16_t port_ = 0;
  pid_t pid_ = -1;
  std::optional<int> exit_code_;  // set once the child is reaped
  bool exit_consumed_ = false;    // a test already looked at exit_code_ via Wait()
};

}  // namespace tideskv::test
