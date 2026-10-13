#pragma once
#include "commands/registry.h"

namespace tideskv {

// Serves the clients blocked on the keys in `ready` (M4 plan F6), then clears it. Runs on the thread that
// ran the command, outside any Engine call, before that command's reply is written. Waiters of a key are
// tried in FIFO order: the scan stops when the key is gone or a waiter would block again, and skips a
// waiter whose blocked type differs from the key's type. A served command may append more keys (BLMOVE's
// destination); they are served in the same call. An exception thrown while re-running a waiter's command
// is logged and ends that waiter's wait with kErrInternal; it never propagates to the caller.
void ServeBlockedClients(const CommandRegistry& registry, ServerServices& server, ReadyKeys& ready);

}  // namespace tideskv
