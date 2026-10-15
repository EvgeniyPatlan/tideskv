#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "common/coding.h"
#include "common/crc32c.h"
#include "encoding/replog_codec.h"
#include "repl/full_sync_frames.h"

namespace {

// A decodable batch has exactly one encoding (the decoder rejects non-minimal varints), so
// re-encoding it must reproduce the input byte for byte.
void CheckCanonical(std::string_view input) {
  auto r = tideskv::encoding::DecodeReplogBatch(input);
  if (!r.ok()) return;
  const std::string again =
      tideskv::encoding::EncodeReplogBatch(r->ops, r->timestamp_ms, r->cum_bytes - input.size());
  if (again != input) __builtin_trap();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  CheckCanonical(input);
  (void)tideskv::repl::DecodeLoadChunk(input);
  // Random bytes almost never carry a valid CRC, which would hide the op parser from the fuzzer:
  // also feed the input with its trailing CRC recomputed.
  if (size >= 4) {
    std::string sealed(input.substr(0, size - 4));
    tideskv::coding::PutFixed32BE(&sealed, tideskv::Crc32c(sealed));
    CheckCanonical(sealed);
  }
  return 0;
}
