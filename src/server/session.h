#pragma once
#include <cstdint>
#include <string>

namespace tideskv {

// Per-connection state. Owned by the connection; touched only on its worker thread.
struct Session {
  uint64_t id = 0;
  std::string peer;                // "ip:port"
  int protocol = 2;                // RESP version, switched by HELLO
  uint8_t db = 0;                  // SELECT (M2)
  bool authenticated = false;      // set true at connect when no password is configured
  bool close_after_reply = false;  // QUIT, SHUTDOWN, protocol errors
  std::string name;                // CLIENT SETNAME
  std::string lib_name;            // CLIENT SETINFO LIB-NAME
  std::string lib_ver;             // CLIENT SETINFO LIB-VER
};

}  // namespace tideskv
