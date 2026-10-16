#include "commands/block_args.h"

#include <cmath>
#include <limits>
#include <string>

#include "common/number_format.h"
#include "common/string_util.h"
#include "types/errors.h"

namespace tideskv {

Status ParseBlockTimeout(std::string_view arg, int64_t now_ms, int64_t* timeout_ms) {
  long double v = 0;
  if (!ParseLongDouble(arg, &v)) return Status::UserError(std::string(kErrTimeoutNotFloat));
  v *= 1000.0L;
  constexpr auto kMax = std::numeric_limits<int64_t>::max();
  const long double ms = std::ceil(v);  // ceil first, so the cast below never overflows
  if (ms > static_cast<long double>(kMax)) return Status::UserError(std::string(kErrTimeoutOutOfRange));
  if (ms < 0) return Status::UserError(std::string(kErrTimeoutNegative));
  const auto t = static_cast<int64_t>(ms);
  if (t > 0 && t > kMax - now_ms) return Status::UserError(std::string(kErrTimeoutOutOfRange));
  *timeout_ms = t;
  return Status::Ok();
}

Status ParseMsTimeout(std::string_view arg, int64_t now_ms, int64_t* ms) {
  if (!ParseInt64(arg, ms)) return Status::UserError(std::string(kErrTimeoutNotInteger));
  if (*ms < 0) return Status::UserError(std::string(kErrTimeoutNegative));
  if (*ms > 0 && *ms > std::numeric_limits<int64_t>::max() - now_ms) {
    return Status::UserError(std::string(kErrTimeoutOutOfRange));
  }
  return Status::Ok();
}

}  // namespace tideskv
