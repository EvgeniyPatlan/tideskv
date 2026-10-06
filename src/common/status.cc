#include "common/status.h"

namespace tideskv {

std::string_view StatusCodeName(StatusCode code) {
  switch (code) {
    case StatusCode::kOk:
      return "OK";
    case StatusCode::kNotFound:
      return "NotFound";
    case StatusCode::kInvalidArgument:
      return "InvalidArgument";
    case StatusCode::kBusy:
      return "Busy";
    case StatusCode::kConflict:
      return "Conflict";
    case StatusCode::kNoSpace:
      return "NoSpace";
    case StatusCode::kIOError:
      return "IOError";
    case StatusCode::kCorruption:
      return "Corruption";
    case StatusCode::kInternal:
      return "Internal";
  }
  return "Unknown";
}

std::string Status::ToString() const {
  std::string out(StatusCodeName(code_));
  if (!message_.empty()) {
    out += ": ";
    out += message_;
  }
  return out;
}

}  // namespace tideskv
