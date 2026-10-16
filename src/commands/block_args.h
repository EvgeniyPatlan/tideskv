#pragma once
#include <cstdint>
#include <string_view>

#include "common/status.h"

namespace tideskv {

// Redis 7.2 getTimeoutFromObjectOrReply(UNIT_SECONDS) (redis72-list.md §0.3): seconds as a long double,
// times 1000, rounded up. *timeout_ms is relative; 0 means block forever. now_ms is only used for the
// overflow check of the absolute deadline Redis computes.
Status ParseBlockTimeout(std::string_view arg, int64_t now_ms, int64_t* timeout_ms);

// getTimeoutFromObjectOrReply(UNIT_MILLISECONDS) (timeout.c:165-196): WAIT and CLIENT PAUSE. *ms is relative;
// UserError with Redis's text (not an integer / negative / the absolute deadline would overflow).
Status ParseMsTimeout(std::string_view arg, int64_t now_ms, int64_t* ms);

}  // namespace tideskv
