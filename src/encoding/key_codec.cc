#include "encoding/key_codec.h"

#include <cassert>
#include <limits>

#include "common/coding.h"
#include "common/crc16.h"
#include "encoding/key_layout.h"

namespace tideskv::encoding {
namespace {

bool DecodeDb(std::string_view* in, uint8_t* db) {
  if (in->empty() || static_cast<uint8_t>((*in)[0]) >= kNumDatabases) return false;
  *db = static_cast<uint8_t>((*in)[0]);
  in->remove_prefix(1);
  return true;
}

// Slotless: no slot bytes, *slot = 0.
bool DecodeSlot(std::string_view* in, uint16_t* slot) {
  if (Slotless()) {
    *slot = 0;
    return true;
  }
  return coding::GetFixed16BE(in, slot) && *slot < kSlotCount;
}

void PutSlot(std::string* out, uint16_t slot) {
  if (!Slotless()) coding::PutFixed16BE(out, slot);
}

}  // namespace

std::string DbPrefix(uint8_t db) { return std::string(1, static_cast<char>(db)); }

std::string MetaSlotPrefix(uint8_t db, uint16_t slot) {
  assert(!Slotless());  // only cluster mode groups keys by slot
  std::string out = DbPrefix(db);
  coding::PutFixed16BE(&out, slot);
  return out;
}

std::string EncodeMetaKey(uint8_t db, uint16_t slot, std::string_view user_key) {
  std::string out;
  out.reserve(3 + user_key.size());
  out.push_back(static_cast<char>(db));
  PutSlot(&out, slot);
  out.append(user_key);
  return out;
}

std::string MetaKeyFor(uint8_t db, std::string_view user_key) {
  return EncodeMetaKey(db, LayoutSlot(user_key), user_key);
}

bool DecodeMetaKey(std::string_view in, MetaKey* out) {
  uint8_t db = 0;
  uint16_t slot = 0;
  if (!DecodeDb(&in, &db) || !DecodeSlot(&in, &slot)) return false;
  out->db = db;
  out->slot = slot;
  out->user_key = in;
  return true;
}

std::string EncodeExpireKey(uint8_t db, uint64_t expire_ms, uint16_t slot, std::string_view user_key) {
  std::string out;
  out.reserve(11 + user_key.size());
  out.push_back(static_cast<char>(db));
  coding::PutFixed64BE(&out, expire_ms);
  PutSlot(&out, slot);
  out.append(user_key);
  return out;
}

std::string ExpireKeyFor(uint8_t db, uint64_t expire_ms, std::string_view user_key) {
  return EncodeExpireKey(db, expire_ms, LayoutSlot(user_key), user_key);
}

std::string ExpireUpperBound(uint8_t db, uint64_t now_ms) {
  if (now_ms == std::numeric_limits<uint64_t>::max()) return DbPrefix(static_cast<uint8_t>(db + 1));
  std::string out = DbPrefix(db);
  coding::PutFixed64BE(&out, now_ms + 1);
  return out;
}

bool DecodeExpireKey(std::string_view in, ExpireKey* out) {
  uint8_t db = 0;
  uint64_t ts = 0;
  uint16_t slot = 0;
  if (!DecodeDb(&in, &db) || !coding::GetFixed64BE(&in, &ts) || !DecodeSlot(&in, &slot)) return false;
  out->db = db;
  out->expire_ms = ts;
  out->slot = slot;
  out->user_key = in;
  return true;
}

std::string PrefixUpperBound(std::string_view prefix) {
  std::string out(prefix);
  while (!out.empty() && static_cast<uint8_t>(out.back()) == 0xff) out.pop_back();
  if (!out.empty()) out.back() = static_cast<char>(static_cast<uint8_t>(out.back()) + 1);
  return out;
}

}  // namespace tideskv::encoding
