#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// The ONLY place that builds or parses oid-bearing keys (spec §4.3, §8.4).
//   subkey key: [db u8][slot u16][oid u64][field]
//   zscore key: [db u8][slot u16][oid u64][score 8B sortable][member]
namespace tideskv::encoding {

inline constexpr uint64_t kListInitialIndex = 1ULL << 63;
inline constexpr size_t kObjectPrefixSize = 11;

struct Subkey {
  uint8_t db = 0;
  uint16_t slot = 0;
  uint64_t oid = 0;
  std::string_view field;  // views into the encoded input
};

struct ZScoreKey {
  uint8_t db = 0;
  uint16_t slot = 0;
  uint64_t oid = 0;
  double score = 0;
  std::string_view member;  // views into the encoded input
};

std::string SubkeyPrefix(uint8_t db, uint16_t slot, uint64_t oid);
std::string EncodeSubkey(uint8_t db, uint16_t slot, uint64_t oid, std::string_view field);
bool DecodeSubkey(std::string_view encoded, Subkey* out);

std::string ZScorePrefix(uint8_t db, uint16_t slot, uint64_t oid);
std::string EncodeZScoreKey(uint8_t db, uint16_t slot, uint64_t oid, double score, std::string_view member);
bool DecodeZScoreKey(std::string_view encoded, ZScoreKey* out);

// M7b D2: the oid of an oid-bearing key (a subkey or zscore key, or their 11-byte DEL_PREFIX prefix), and
// that key with another oid. False when `key` is shorter than kObjectPrefixSize.
bool PeekObjectOid(std::string_view key, uint64_t* oid);
bool RewriteObjectOid(std::string* key, uint64_t oid);

std::string EncodeListIndex(uint64_t index);
bool DecodeListIndex(std::string_view field, uint64_t* index);

}  // namespace tideskv::encoding
