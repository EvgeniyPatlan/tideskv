#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "storage/column_family.h"

// Replication-log batch record, version 1 (M6a D4). One record per committed transaction:
//   header : [version u8][timestamp_ms i64 BE][cum_bytes u64 BE][op_count u32 BE]   (21 bytes)
//   op     : [type u8][cf u8][klen varint][key][vlen varint][value]                 (>= 4 bytes)
//   trailer: [crc32c u32 BE] over header + ops
// cum_bytes is the replication stream size through the end of this record. The decoder rejects
// non-minimal varints, so every decodable record has exactly one encoding.
namespace tideskv::encoding {

enum class ReplOpType : uint8_t { kPut = 1, kDelete = 2, kDeletePrefix = 3, kDeleteRange = 4 };

struct ReplOp {
  ReplOpType type = ReplOpType::kPut;
  ColumnFamily cf = ColumnFamily::kMeta;
  std::string key;    // PUT/DEL: the key; DEL_PREFIX: the prefix; DEL_RANGE: lo
  std::string value;  // PUT: the value; DEL_RANGE: hi (empty = to the end); otherwise empty
  bool operator==(const ReplOp&) const = default;
};

inline constexpr uint8_t kReplogVersion = 1;
inline constexpr size_t kReplogHeaderSize = 1 + 8 + 8 + 4;  // version, timestamp, cum_bytes, op_count

std::string ReplogKey(uint64_t seq);  // 8-byte big-endian
bool DecodeReplogKey(std::string_view key, uint64_t* seq);

// Encodes a batch; cum_bytes_before + the returned size is written as cum_bytes (D4).
std::string EncodeReplogBatch(const std::vector<ReplOp>& ops, int64_t timestamp_ms,
                              uint64_t cum_bytes_before);

struct ReplogBatch {
  int64_t timestamp_ms = 0;
  uint64_t cum_bytes = 0;
  std::vector<ReplOp> ops;
};

Result<ReplogBatch> DecodeReplogBatch(std::string_view encoded);  // Corruption on any defect

// Header-only peek (no op decoding, no CRC): the trimmer reads only these.
bool PeekReplogHeader(std::string_view encoded, int64_t* timestamp_ms, uint64_t* cum_bytes);

}  // namespace tideskv::encoding
