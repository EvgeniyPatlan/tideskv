#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// Layouts (spec §4.1). Integers are big-endian.
//   meta   key: [slot u16][db u8][user_key]
//   expire key: [expire_ms u64][slot u16][db u8][user_key]
namespace tideskv::encoding {

struct MetaKey {
  uint16_t slot = 0;
  uint8_t db = 0;
  std::string_view user_key;  // views into the encoded input
};

struct ExpireKey {
  uint64_t expire_ms = 0;
  uint16_t slot = 0;
  uint8_t db = 0;
  std::string_view user_key;  // views into the encoded input
};

std::string EncodeMetaKey(uint16_t slot, uint8_t db, std::string_view user_key);
bool DecodeMetaKey(std::string_view encoded, MetaKey* out);
std::string MetaSlotPrefix(uint16_t slot);
std::string MetaDbPrefix(uint16_t slot, uint8_t db);

std::string EncodeExpireKey(uint64_t expire_ms, uint16_t slot, uint8_t db, std::string_view user_key);
bool DecodeExpireKey(std::string_view encoded, ExpireKey* out);

}  // namespace tideskv::encoding
