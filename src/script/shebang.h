#pragma once
#include <string>
#include <string_view>

#include "common/status.h"

namespace tideskv {

// Script flags from a "#!lua flags=..." first line (eval.c evalExtractShebangFlags, M5b H8).
struct ScriptFlags {
  bool shebang = false;  // the body starts with "#!": not compat mode
  bool no_writes = false;
  bool allow_oom = false;
  bool allow_stale = false;
  bool no_cluster = false;
  bool allow_cross_slot = false;
};

struct ParsedScript {
  ScriptFlags flags;
  std::string code;  // what Lua compiles: the shebang line removed, its newline kept
};

// UserError with Redis's text for a bad shebang.
Result<ParsedScript> ParseShebang(std::string_view body);

}  // namespace tideskv
