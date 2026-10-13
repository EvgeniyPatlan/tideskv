#include "commands/count_args.h"

#include <limits>
#include <string>

#include "common/string_util.h"
#include "types/errors.h"

namespace tideskv {

Status ParseRandomCount(std::string_view arg, int64_t* out) {
  if (!ParseInt64(arg, out)) return Status::UserError(std::string(kErrNotInteger));
  if (*out == std::numeric_limits<int64_t>::min()) return Status::UserError(std::string(kErrCountOutOfRange));
  return Status::Ok();
}

Status ParsePositiveCount(std::string_view arg, std::string_view error, int64_t* out) {
  if (!ParseInt64(arg, out) || *out < 0) return Status::UserError(std::string(error));
  return Status::Ok();
}

Status ParseNonNegativeCount(std::string_view arg, int64_t* out) {
  return ParsePositiveCount(arg, kErrMustBePositive, out);
}

}  // namespace tideskv
