#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace tideskv {

bool ParseInt64(std::string_view s, int64_t* out);
bool ParseUint64(std::string_view s, uint64_t* out);
// Redis memory notation: k=1000 kb=1024 m=10^6 mb=2^20 g=10^9 gb=2^30 (case-insensitive).
bool ParseMemory(std::string_view s, uint64_t* out);
std::string ToLower(std::string_view s);
bool EqualsIgnoreCase(std::string_view a, std::string_view b);
// Comparison whose duration depends only on the input lengths (for passwords).
bool ConstantTimeEquals(std::string_view a, std::string_view b);

}  // namespace tideskv
