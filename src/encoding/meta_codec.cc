#include "encoding/meta_codec.h"

#include "common/coding.h"

namespace tideskv::encoding {
namespace {

constexpr size_t kHeaderSize = 10;
constexpr size_t kCollectionSize = kHeaderSize + 16;
constexpr size_t kListSize = kCollectionSize + 16;

bool IsKnownType(uint8_t t) { return t >= 1 && t <= 5; }

size_t ExpectedSize(ValueType type) { return type == ValueType::kList ? kListSize : kCollectionSize; }

}  // namespace

std::string_view TypeName(ValueType type) {
  switch (type) {
    case ValueType::kString:
      return "string";
    case ValueType::kHash:
      return "hash";
    case ValueType::kList:
      return "list";
    case ValueType::kSet:
      return "set";
    case ValueType::kZSet:
      return "zset";
  }
  return "unknown";
}

std::string EncodeMetadata(const Metadata& m, std::string_view string_payload) {
  std::string out;
  out.reserve(kListSize + string_payload.size());
  out.push_back(static_cast<char>(m.type));
  out.push_back(static_cast<char>(m.flags));
  coding::PutFixed64BE(&out, static_cast<uint64_t>(m.expire_ms));
  if (m.type == ValueType::kString) {
    out.append(string_payload);
    return out;
  }
  coding::PutFixed64BE(&out, m.oid);
  coding::PutFixed64BE(&out, m.size);
  if (m.type == ValueType::kList) {
    coding::PutFixed64BE(&out, m.head);
    coding::PutFixed64BE(&out, m.tail);
  }
  return out;
}

Result<DecodedMetadata> DecodeMetadata(std::string_view in) {
  if (in.size() < kHeaderSize) return Status::Corruption("metadata too short");
  const auto raw_type = static_cast<uint8_t>(in[0]);
  if (!IsKnownType(raw_type)) return Status::Corruption("unknown value type " + std::to_string(raw_type));
  DecodedMetadata d;
  d.meta.type = static_cast<ValueType>(raw_type);
  d.meta.flags = static_cast<uint8_t>(in[1]);
  d.meta.expire_ms = static_cast<int64_t>(coding::DecodeFixed64BE(in.data() + 2));

  if (d.meta.type == ValueType::kString) {
    d.payload = in.substr(kHeaderSize);
    return d;
  }
  if (in.size() != ExpectedSize(d.meta.type)) return Status::Corruption("bad metadata length");
  d.meta.oid = coding::DecodeFixed64BE(in.data() + kHeaderSize);
  d.meta.size = coding::DecodeFixed64BE(in.data() + kHeaderSize + 8);
  if (d.meta.type == ValueType::kList) {
    d.meta.head = coding::DecodeFixed64BE(in.data() + kCollectionSize);
    d.meta.tail = coding::DecodeFixed64BE(in.data() + kCollectionSize + 8);
  }
  return d;
}

}  // namespace tideskv::encoding
