#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace tideskv {

class BlockedClient;  // server/blocking.h

// How another thread reaches a connection: implemented by net::Connection (M4 plan F5).
class ClientLink {
 public:
  virtual ~ClientLink() = default;
  // Thread-safe and non-blocking. Called once when this client's blocked command ends on another thread
  // (served by a push, or CLIENT UNBLOCK): the connection appends `reply` (encoded bytes) and resumes
  // executing its pipelined input.
  virtual void Unblock(std::string reply) = 0;
};

// Per-connection state. Owned by the connection; touched only on its worker thread.
struct Session {
  uint64_t id = 0;
  std::string peer;                        // "ip:port"
  int protocol = 2;                        // RESP version, switched by HELLO
  uint8_t db = 0;                          // SELECT (M2)
  bool authenticated = false;              // set true at connect when no password is configured
  bool close_after_reply = false;          // QUIT, SHUTDOWN, protocol errors
  std::string name;                        // CLIENT SETNAME
  std::string lib_name;                    // CLIENT SETINFO LIB-NAME
  std::string lib_ver;                     // CLIENT SETINFO LIB-VER
  std::weak_ptr<ClientLink> link;          // set by the connection when it starts; empty in unit tests
  std::shared_ptr<BlockedClient> blocked;  // set while a blocking command waits (M4 F5)
};

}  // namespace tideskv
