#include "encoding/object_codec.h"

#include "common/coding.h"
#include "encoding/score_codec.h"

namespace tideskv::encoding {
namespace {

bool DecodePrefix(std::string_view* in, uint16_t* slot, uint64_t* oid) {
  return coding::GetFixed16BE(in, slot) && coding::GetFixed64BE(in, oid);
}

}  // namespace

std::string SubkeyPrefix(uint16_t slot, uint64_t oid) {
  std::string out;
  out.reserve(kObjectPrefixSize);
  coding::PutFixed16BE(&out, slot);
  coding::PutFixed64BE(&out, oid);
  return out;
}

std::string EncodeSubkey(uint16_t slot, uint64_t oid, std::string_view field) {
  std::string out = SubkeyPrefix(slot, oid);
  out.append(field);
  return out;
}

bool DecodeSubkey(std::string_view in, Subkey* out) {
  if (!DecodePrefix(&in, &out->slot, &out->oid)) return false;
  out->field = in;
  return true;
}

std::string ZScorePrefix(uint16_t slot, uint64_t oid) { return SubkeyPrefix(slot, oid); }

std::string EncodeZScoreKey(uint16_t slot, uint64_t oid, double score, std::string_view member) {
  std::string out = SubkeyPrefix(slot, oid);
  AppendScore(&out, score);
  out.append(member);
  return out;
}

bool DecodeZScoreKey(std::string_view in, ZScoreKey* out) {
  if (!DecodePrefix(&in, &out->slot, &out->oid) || in.size() < 8) return false;
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

}  // namespace tideskv::encoding
