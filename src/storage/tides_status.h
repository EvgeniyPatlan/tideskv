#pragma once
#include <string_view>

#include "common/status.h"

namespace tideskv {

// Maps a TidesDB return code (TDB_*) to a Status. `context` names the operation
// and is included in the message (for logs only, never shown to clients).
Status FromTdb(int rc, std::string_view context);

}  // namespace tideskv
