#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// Layouts (spec §4.1 as amended by the M2 plan, D1). Integers are big-endian.
//   meta   key: [db u8][slot u16][user_key]
//   expire key: [db u8][expire_ms u64][slot u16][user_key]
// The db byte comes first in every keyspace CF, so one db is one contiguous prefix. With the slotless layout
// (key_layout.h) the slot u16 is absent: every `slot` argument is ignored and every decoded `slot` is 0.
namespace tideskv::encoding {

inline constexpr uint8_t kNumDatabases = 16;

struct MetaKey {
  uint8_t db = 0;
  uint16_t slot = 0;          // meaningless (0) when slotless
  std::string_view user_key;  // views into the encoded input
};

struct ExpireKey {
  uint8_t db = 0;
  uint64_t expire_ms = 0;
  uint16_t slot = 0;          // meaningless (0) when slotless
  std::string_view user_key;  // views into the encoded input
};

std::string DbPrefix(uint8_t db);
std::string EncodeMetaKey(uint8_t db, uint16_t slot, std::string_view user_key);
std::string MetaKeyFor(uint8_t db, std::string_view user_key);  // slot = LayoutSlot(user_key)
bool DecodeMetaKey(std::string_view encoded, MetaKey* out);
std::string MetaSlotPrefix(uint8_t db, uint16_t slot);  // slotted only (cluster mode)

std::string EncodeExpireKey(uint8_t db, uint64_t expire_ms, uint16_t slot, std::string_view user_key);
std::string ExpireKeyFor(uint8_t db, uint64_t expire_ms, std::string_view user_key);
// Exclusive upper bound of the entries of `db` that are due at now_ms (expire_ms <= now_ms).
std::string ExpireUpperBound(uint8_t db, uint64_t now_ms);
bool DecodeExpireKey(std::string_view encoded, ExpireKey* out);

// Smallest string greater than every string starting with `prefix`, for an exclusive iterator
// bound: trailing 0xff bytes are dropped and the last remaining byte is incremented. Empty when
// no such bound exists (all bytes 0xff, or an empty prefix): iterate to the end.
std::string PrefixUpperBound(std::string_view prefix);

}  // namespace tideskv::encoding
