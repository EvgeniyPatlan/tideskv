#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"

namespace tideskv::resp {

// One RESP2/RESP3 reply as a tree (M5b H5): what a command handler wrote, read back for Lua.
struct ReplyNode {
  enum class Type {
    kStatus,
    kError,
    kInteger,
    kBulk,
    kNullBulk,
    kNullArray,
    kArray,
    kNull,
    kBool,
    kDouble,
    kMap,
    kSet,
    kBigNumber,
    kVerbatim
  };
  Type type = Type::kNull;
  std::string str;     // status, error (without '-'), bulk, big number digits, verbatim text
  std::string format;  // verbatim: the 3-character format
  int64_t integer = 0;
  double dbl = 0;
  bool boolean = false;
  std::vector<ReplyNode> elems;  // array/set elements; map: k1, v1, k2, v2, ...
};

// Parses exactly one complete reply (attributes are skipped). Internal on anything else:
// the input always comes from our own ReplyBuilder.
Result<ReplyNode> ParseReply(std::string_view data);

}  // namespace tideskv::resp
