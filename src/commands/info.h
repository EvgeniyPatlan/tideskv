#pragma once
#include <string>
#include <vector>

#include "commands/server_services.h"

namespace tideskv {

// Redis-style INFO text. Empty / "default" / "all" / "everything" selects every section.
std::string FormatInfo(const ServerStats& stats, const std::vector<std::string>& sections);

}  // namespace tideskv
