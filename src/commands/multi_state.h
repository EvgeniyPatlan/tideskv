#pragma once
#include "commands/registry.h"

namespace tideskv {

// Drops the session's watches and clears its dirty flag (no-op if it never watched).
void UnwatchClient(ExecContext& ctx);
// Redis discardTransaction (multi.c:98): leaves MULTI, empties the queue, clears both dirty
// flags and unwatches. Used by EXEC, DISCARD, RESET and a rejected EXEC.
void DiscardTransaction(ExecContext& ctx);

}  // namespace tideskv
