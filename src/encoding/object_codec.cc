#include "encoding/object_codec.h"

#include "common/coding.h"
#include "encoding/score_codec.h"

namespace tideskv::encoding {
namespace {

bool DecodePrefix(std::string_view* in, uint8_t* db, uint16_t* slot, uint64_t* oid) {
  if (in->empty()) return false;
  *db = static_cast<uint8_t>((*in)[0]);
  in->remove_prefix(1);
  return coding::GetFixed16BE(in, slot) && coding::GetFixed64BE(in, oid);
}

}  // namespace

std::string SubkeyPrefix(uint8_t db, uint16_t slot, uint64_t oid) {
  std::string out;
  out.reserve(kObjectPrefixSize);
  out.push_back(static_cast<char>(db));
  coding::PutFixed16BE(&out, slot);
  coding::PutFixed64BE(&out, oid);
  return out;
}

std::string EncodeSubkey(uint8_t db, uint16_t slot, uint64_t oid, std::string_view field) {
  std::string out = SubkeyPrefix(db, slot, oid);
  out.append(field);
  return out;
}

bool DecodeSubkey(std::string_view in, Subkey* out) {
  if (!DecodePrefix(&in, &out->db, &out->slot, &out->oid)) return false;
  out->field = in;
  return true;
}

std::string ZScorePrefix(uint8_t db, uint16_t slot, uint64_t oid) { return SubkeyPrefix(db, slot, oid); }

std::string EncodeZScoreKey(uint8_t db, uint16_t slot, uint64_t oid, double score, std::string_view member) {
  std::string out = SubkeyPrefix(db, slot, oid);
  AppendScore(&out, score);
  out.append(member);
  return out;
}

bool DecodeZScoreKey(std::string_view in, ZScoreKey* out) {
  if (!DecodePrefix(&in, &out->db, &out->slot, &out->oid) || in.size() < 8) return false;
  out->score = DecodeScore(in);
  out->member = in.substr(8);
  return true;
}

std::string EncodeListIndex(uint64_t index) {
  std::string out;
  coding::PutFixed64BE(&out, index);
  return out;
}

bool DecodeListIndex(std::string_view field, uint64_t* index) {
  return field.size() == 8 && coding::GetFixed64BE(&field, index);
}

bool PeekObjectOid(std::string_view key, uint64_t* oid) {
  if (key.size() < kObjectPrefixSize) return false;
  *oid = coding::DecodeFixed64BE(key.data() + 3);  // after [db u8][slot u16]
  return true;
}

bool RewriteObjectOid(std::string* key, uint64_t oid) {
  if (key->size() < kObjectPrefixSize) return false;
  std::string be;
  coding::PutFixed64BE(&be, oid);
  key->replace(3, 8, be);
  return true;
}

}  // namespace tideskv::encoding
