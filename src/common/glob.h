#pragma once
#include <string_view>

namespace tideskv {

// Redis-compatible glob: '*', '?', '[abc]', '[^abc]', '[a-z]', '\x' escapes a char.
// Single-backtrack-point algorithm: O(|pattern| * |str|), never exponential.
bool GlobMatch(std::string_view pattern, std::string_view str, bool nocase = false);

}  // namespace tideskv
