#include "commands/scan_args.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>

#include "common/string_util.h"
#include "types/errors.h"

namespace tideskv {

bool ParseScanCursor(const std::string& arg, uint64_t* cursor) {
  const char* begin = arg.c_str();
  if (std::isspace(static_cast<unsigned char>(begin[0]))) return false;
  char* end = nullptr;
  errno = 0;
  const unsigned long long v = std::strtoull(begin, &end, 10);
  if (*end != '\0' || errno == ERANGE) return false;
  *cursor = v;
  return true;
}

Status ParseScanOptions(const ArgList& argv, size_t first, bool allow_type, ScanOptions* out) {
  for (size_t i = first; i < argv.size(); i += 2) {
    if (i + 1 >= argv.size()) return Status::UserError(std::string(kErrSyntax));
    const std::string& value = argv[i + 1];
    if (EqualsIgnoreCase(argv[i], "COUNT")) {
      int64_t n = 0;
      if (!ParseInt64(value, &n)) return Status::UserError(std::string(kErrNotInteger));
      if (n < 1) return Status::UserError(std::string(kErrSyntax));
      out->count = static_cast<size_t>(n);
    } else if (EqualsIgnoreCase(argv[i], "MATCH")) {
      // Redis 7.2 scanGenericCommand: use_pattern unless the pattern is exactly "*".
      out->filter.pattern = value == "*" ? std::nullopt : std::optional<std::string_view>(value);
    } else if (allow_type && EqualsIgnoreCase(argv[i], "TYPE")) {
      out->filter.type = TypeFromName(value);
      out->filter.match_nothing = !out->filter.type.has_value();
    } else {
      return Status::UserError(std::string(kErrSyntax));
    }
  }
  return Status::Ok();
}

}  // namespace tideskv
