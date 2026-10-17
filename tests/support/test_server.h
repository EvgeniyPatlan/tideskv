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
    std::string config_file;                               // passed first (tideskv-server <file>); "" = none
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
  // SIGSTOP / SIGCONT: freezes every thread of the server (its reaper, its replication threads) and
  // thaws it. Pause returns once every thread is stopped. Terminate and the destructor resume a paused
  // server first.
  void Pause();
  void Resume();
  // Starts the server again on the same dir and port (after Kill9 or Terminate, or stopping it first),
  // with the original options or with `extra_args` replacing them; waits until it is ready.
  void Restart();
  void Restart(std::vector<std::string> extra_args);
  pid_t pid() const { return pid_; }
  bool IsRunning();
  std::string Stderr() const;  // every run's log, Restart included
  // Stderr() from the latest (re)start on: a restarted server's log lines, without its predecessors'.
  std::string StderrSinceStart() const;
  // The first sanitizer report in Stderr(), from its first line on (at most 64 KiB), or "".
  std::string SanitizerReport() const;

 private:
  static int DecodeStatus(int status);
  void Spawn(const std::string& binary, const Options& options);
  bool WaitReady(std::chrono::milliseconds timeout);
  void SpawnUntilReady();
  static uint16_t FreePort();

  TempDir log_dir_{"tideskv-it-log"};
  std::string binary_;
  Options options_;
  size_t log_start_ = 0;  // Stderr() offset where the latest run's log begins
  std::unique_ptr<TempDir> owned_dir_;
  std::string dir_;
  uint16_t port_ = 0;
  pid_t pid_ = -1;
  std::optional<int> exit_code_;  // set once the child is reaped
  bool exit_consumed_ = false;    // a test already looked at exit_code_ via Wait()
  bool paused_ = false;           // SIGSTOP sent and not yet followed by SIGCONT
};

}  // namespace tideskv::test
