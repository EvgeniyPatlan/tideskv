#include "storage/validation.h"

#include "storage/storage.h"

namespace tideskv {

Status ValidateKey(std::string_view key) {
  return key.empty() ? Status::InvalidArgument("key must not be empty") : Status::Ok();
}

Status ValidatePrefix(std::string_view prefix) {
  if (prefix.empty() || prefix.size() > kMaxRangeBoundSize) {
    return Status::InvalidArgument("prefix must be 1..256 bytes");
  }
  return Status::Ok();
}

Status ValidateRange(std::string_view lo, std::string_view hi) {
  if (lo.empty() || lo.size() > kMaxRangeBoundSize || hi.size() > kMaxRangeBoundSize) {
    return Status::InvalidArgument("range bounds must be 1..256 (lo) and 0..256 (hi) bytes");
  }
  if (!hi.empty() && !(lo < hi)) return Status::InvalidArgument("range lo must be < hi");
  return Status::Ok();
}

Status ValidateSavepointName(std::string_view name) {
  if (name.empty() || name.find('\0') != std::string_view::npos) {
    return Status::InvalidArgument("savepoint name must be non-empty and contain no NUL");
  }
  return Status::Ok();
}

}  // namespace tideskv
