#pragma once
#include <string_view>

#include "common/status.h"
#include "storage/storage.h"

namespace tideskv {

inline constexpr std::string_view kFormatVersionKey = "format_version";  // in the sys CF
inline constexpr std::string_view kFormatVersion = "1";                  // M2 plan D1/D2 layout

// Writes the version into a fresh store; Corruption if the store has a different one.
Status EnsureFormatVersion(Storage& storage);

}  // namespace tideskv
