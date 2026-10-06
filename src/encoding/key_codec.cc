#include "encoding/key_codec.h"

#include "common/coding.h"
#include "common/crc16.h"

namespace tideskv::encoding {

std::string MetaSlotPrefix(uint16_t slot) {
  std::string out;
  coding::PutFixed16BE(&out, slot);
  return out;
}

std::string MetaDbPrefix(uint16_t slot, uint8_t db) {
  std::string out = MetaSlotPrefix(slot);
  out.push_back(static_cast<char>(db));
  return out;
}

std::string EncodeMetaKey(uint16_t slot, uint8_t db, std::string_view user_key) {
  std::string out;
  out.reserve(3 + user_key.size());
  coding::PutFixed16BE(&out, slot);
  out.push_back(static_cast<char>(db));
  out.append(user_key);
  return out;
}

bool DecodeMetaKey(std::string_view in, MetaKey* out) {
  uint16_t slot = 0;
  if (!coding::GetFixed16BE(&in, &slot) || slot >= kSlotCount || in.empty()) return false;
  out->slot = slot;
  out->db = static_cast<uint8_t>(in[0]);
  out->user_key = in.substr(1);
  return true;
}

std::string EncodeExpireKey(uint64_t expire_ms, uint16_t slot, uint8_t db, std::string_view user_key) {
  std::string out;
  out.reserve(11 + user_key.size());
  coding::PutFixed64BE(&out, expire_ms);
  coding::PutFixed16BE(&out, slot);
  out.push_back(static_cast<char>(db));
  out.append(user_key);
  return out;
}

bool DecodeExpireKey(std::string_view in, ExpireKey* out) {
  uint64_t ts = 0;
  if (!coding::GetFixed64BE(&in, &ts)) return false;
  MetaKey rest;
  if (!DecodeMetaKey(in, &rest)) return false;
  out->expire_ms = ts;
  out->slot = rest.slot;
  out->db = rest.db;
  out->user_key = rest.user_key;
  return true;
}

}  // namespace tideskv::encoding
