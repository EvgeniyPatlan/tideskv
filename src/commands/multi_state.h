#pragma once
#include <cstdint>
#include <string>

#include "commands/registry.h"

namespace tideskv {

// The connection state a queued command may change (SELECT, HELLO, AUTH, CLIENT SETNAME).
// EXEC restores it when its batch fails to commit: a failed EXEC applies nothing (M5a G1).
struct ConnectionState {
  uint8_t db = 0;
  int protocol = 2;
  bool authenticated = false;
  std::string name;
  static ConnectionState Capture(const Session& s);
  void Restore(Session& s, resp::ReplyBuilder& reply) const;  // also sets reply's protocol
};

// Drops the session's watches and clears its dirty flag (no-op if it never watched).
void UnwatchClient(ExecContext& ctx);
// Redis discardTransaction (multi.c:98): leaves MULTI, empties the queue, clears both dirty
// flags and unwatches. Used by EXEC, DISCARD, RESET and a rejected EXEC.
void DiscardTransaction(ExecContext& ctx);

}  // namespace tideskv
