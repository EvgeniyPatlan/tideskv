#pragma once
#include <string>
#include <string_view>

namespace tideskv {

// SHA1 of `data` as 40 lowercase hex characters (Redis sha1hex, eval.c:112).
std::string Sha1Hex(std::string_view data);

}  // namespace tideskv
