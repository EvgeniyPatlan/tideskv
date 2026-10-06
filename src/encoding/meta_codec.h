#pragma once
#include <cstdint>
#include <string>
#include <string_view>

#include "common/status.h"
#include "encoding/object_codec.h"

// Metadata record stored in the `meta` CF (spec §4.2):
//   header : [type u8][flags u8][expire_ms i64]      (10 bytes, big-endian)
//   STRING : header + payload
//   HASH/SET/ZSET : header + [oid u64][size u64]     (26 bytes)
//   LIST   : header + [oid][size][head u64][tail u64] (42 bytes)
namespace tideskv::encoding {

enum class ValueType : uint8_t { kString = 1, kHash = 2, kList = 3, kSet = 4, kZSet = 5 };

std::string_view TypeName(ValueType type);

struct Metadata {
  ValueType type = ValueType::kString;
  uint8_t flags = 0;
  int64_t expire_ms = 0;                   // absolute unix ms; 0 = never expires
  uint64_t oid = 0;                        // collections only
  uint64_t size = 0;                       // collections only: element count
  uint64_t head = kListInitialIndex;       // list only: index of first element
  uint64_t tail = kListInitialIndex - 1;   // list only: index of last; tail < head ⇒ empty

  bool IsCollection() const { return type != ValueType::kString; }
  bool IsExpired(int64_t now_ms) const { return expire_ms > 0 && expire_ms <= now_ms; }
};

struct DecodedMetadata {
  Metadata meta;
  std::string_view payload;  // STRING value; views into the encoded input
};

std::string EncodeMetadata(const Metadata& meta, std::string_view string_payload = {});
Result<DecodedMetadata> DecodeMetadata(std::string_view encoded);

}  // namespace tideskv::encoding
