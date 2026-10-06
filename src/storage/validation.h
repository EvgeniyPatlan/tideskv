#pragma once
#include <string_view>

#include "common/status.h"

// Argument checks shared by every Storage backend so they reject the same inputs.
namespace tideskv {

Status ValidateKey(std::string_view key);                        // non-empty
Status ValidatePrefix(std::string_view prefix);                  // 1..256 bytes
Status ValidateRange(std::string_view lo, std::string_view hi);  // lo 1..256, hi 0..256, lo < hi
Status ValidateSavepointName(std::string_view name);             // non-empty, no NUL

}  // namespace tideskv
