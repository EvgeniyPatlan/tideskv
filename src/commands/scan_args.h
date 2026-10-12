#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "commands/registry.h"
#include "common/status.h"
#include "types/scan.h"

// SCAN-family argument parsing (Redis 7.2 parseScanCursorOrReply / scanGenericCommand).
namespace tideskv {

struct ScanOptions {
  size_t count = 10;
  ScanFilter filter;  // pattern views into argv
};

// strtoul semantics: '-1' wraps, '+5', '007' and '' parse; leading whitespace, trailing junk
// and out-of-range values are rejected.
bool ParseScanCursor(const std::string& arg, uint64_t* cursor);
// Options from argv[first] on: COUNT n (n >= 1), MATCH pattern, and TYPE name only when
// allow_type (SCAN). Anything else is UserError(kErrSyntax).
Status ParseScanOptions(const ArgList& argv, size_t first, bool allow_type, ScanOptions* out);

}  // namespace tideskv
