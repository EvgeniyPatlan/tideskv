#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"

// The migrator's request/response session with the target (M7b D3): AUTH, CLUSTER IMPORT, IMPORTDATA. An
// interface so unit tests run the migrator against an in-process importer without sockets.
namespace tideskv::migrate {

class MigrationTransport {
 public:
  virtual ~MigrationTransport() = default;
  // One request and its single-line reply: +... -> Ok; -<text> -> UserError(<text>); I/O or a protocol error
  // -> IOError/TimedOut. Called from the migrator thread only.
  virtual Status Call(const std::vector<std::string_view>& args) = 0;
  // Any thread: a Call in progress, and every later one, fails promptly.
  virtual void Shutdown() = 0;
};

using TransportFactory = std::function<Result<std::unique_ptr<MigrationTransport>>(
    const std::string& host, uint16_t port, std::chrono::milliseconds timeout,
    const std::atomic<bool>* cancel)>;

// The real transport: repl::ConnectTcp (cancellable) + repl::RespConn, every call bounded by `timeout`.
Result<std::unique_ptr<MigrationTransport>> ConnectResp(const std::string& host, uint16_t port,
                                                        std::chrono::milliseconds timeout,
                                                        const std::atomic<bool>* cancel);

}  // namespace tideskv::migrate
