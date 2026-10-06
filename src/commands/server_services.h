#pragma once
#include <cstdint>
#include <string>

#include "server/config.h"

namespace tideskv {

class CommandRegistry;

struct ServerStats {
  std::string version;
  int64_t pid = 0;
  uint16_t tcp_port = 0;
  int64_t uptime_seconds = 0;
  int workers = 0;
  int maxclients = 0;
  uint64_t connected_clients = 0;
  uint64_t total_connections = 0;
  uint64_t total_commands = 0;
  std::string data_dir;
  std::string sync_mode;
};

// What command handlers may ask of the server. Implemented by App (Task 19) and
// by test::FakeServerServices.
class ServerServices {
 public:
  virtual ~ServerServices() = default;
  virtual Config& config() = 0;
  virtual const CommandRegistry& registry() const = 0;
  virtual ServerStats Stats() const = 0;
  virtual void ResetStats() = 0;
  virtual void RequestShutdown() = 0;  // must not block; the main thread performs the shutdown
};

}  // namespace tideskv
