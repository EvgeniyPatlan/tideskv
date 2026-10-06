// libFuzzer target: the parser must never crash, hang, over-consume, or emit an empty command.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "resp/request_parser.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using tideskv::resp::ParseStatus;
  tideskv::resp::ParserLimits limits;
  limits.max_bulk_len = 1 << 20;
  limits.max_multibulk_len = 1024;
  limits.max_inline_len = 4096;
  tideskv::resp::RequestParser parser(limits);
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  std::string pending;
  const size_t half = size / 2;  // two chunks exercise partial-state handling
  for (std::string_view chunk : {input.substr(0, half), input.substr(half)}) {
    pending.append(chunk);
    for (;;) {
      size_t consumed = 0;
      const ParseStatus st = parser.Parse(pending, &consumed);
      if (consumed > pending.size()) __builtin_trap();
      pending.erase(0, consumed);
      if (st == ParseStatus::kError) return 0;
      if (st == ParseStatus::kNeedMore) break;
      if (parser.args().empty()) __builtin_trap();
    }
  }
  return 0;
}
