#include "types/digest.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "common/coding.h"
#include "common/crc16.h"
#include "common/score_format.h"
#include "common/sha1.h"
#include "encoding/key_codec.h"
#include "encoding/meta_codec.h"
#include "types/collection.h"
#include "types/list_type.h"
#include "types/slot_keys.h"
#include "types/zset_type.h"

namespace tideskv {
namespace {

using encoding::ValueType;

constexpr uint64_t kListChunk = 512;  // list elements read per range read

std::string_view Bytes(const Digest20& d) { return {reinterpret_cast<const char*>(d.data()), d.size()}; }

// debug.c xorDigest: d ^= SHA1(bytes). Order-free "add".
void XorDigest(Digest20& d, std::string_view bytes) {
  const Sha1Digest h = Sha1(bytes);
  for (size_t i = 0; i < d.size(); ++i) d[i] ^= h[i];
}

// debug.c mixDigest: d = SHA1(d ^ SHA1(bytes)). Order-dependent.
void MixDigest(Digest20& d, std::string_view bytes) {
  XorDigest(d, bytes);
  d = Sha1(Bytes(d));
}

void MixUint32(Digest20& d, uint32_t v) {  // mixDigest(d, htonl(v), 4)
  std::string be;
  coding::PutFixed32BE(&be, v);
  MixDigest(d, be);
}

// Redis object type codes (server.h OBJ_*).
uint32_t RedisTypeCode(ValueType type) {
  switch (type) {
    case ValueType::kString:
      return 0;
    case ValueType::kList:
      return 1;
    case ValueType::kSet:
      return 2;
    case ValueType::kZSet:
      return 3;
    case ValueType::kHash:
      return 4;
  }
  return 0;
}

Status DigestList(ReadView& view, const Collection& c, Digest20& d) {
  std::vector<std::string> chunk;
  for (uint64_t from = 0; from < c.size(); from += kListChunk) {
    chunk.clear();
    const uint64_t to = std::min(c.size(), from + kListChunk) - 1;
    if (Status st = ReadListRange(view, c, from, to, &chunk); !st.ok()) return st;
    for (const auto& e : chunk) MixDigest(d, e);
  }
  return Status::Ok();
}

Status DigestSet(ReadView& view, const Collection& c, Digest20& d) {
  return ForEachElement(view, c, [&d](std::string_view member, std::string_view) {
    XorDigest(d, member);
    return true;
  });
}

// One element of a zset or hash: e = 0; mix(e, name); mix(e, value); d ^= SHA1(e).
void XorPairDigest(Digest20& d, std::string_view name, std::string_view value) {
  Digest20 e{};
  MixDigest(e, name);
  MixDigest(e, value);
  XorDigest(d, Bytes(e));
}

Status DigestZset(ReadView& view, const Collection& c, Digest20& d) {
  bool corrupt = false;
  Status st = ForEachElement(view, c, [&](std::string_view member, std::string_view raw) {
    if (raw.size() != 8) {
      corrupt = true;
      return false;
    }
    XorPairDigest(d, member, FormatDtoa(DecodeRawScore(raw)));  // debug.c: fpconv_dtoa, not d2string
    return true;
  });
  if (!st.ok()) return st;
  return corrupt ? Status::Corruption("zset member score is not 8 bytes") : Status::Ok();
}

Status DigestHash(ReadView& view, const Collection& c, Digest20& d) {
  return ForEachElement(view, c, [&d](std::string_view field, std::string_view value) {
    XorPairDigest(d, field, value);
    return true;
  });
}

// debug.c xorObjectDigest for the stored object (db, slot, key) whose meta record is `m`; continues from d.
Status XorObjectDigest(ReadView& view, uint8_t db, uint16_t slot, std::string_view key,
                       const encoding::DecodedMetadata& m, Digest20& d) {
  MixUint32(d, RedisTypeCode(m.meta.type));
  Status st = Status::Ok();
  if (m.meta.type == ValueType::kString) {
    MixDigest(d, m.payload);
  } else {
    // Built from the meta record directly: LoadCollection would hide a logically expired key.
    Collection c{db, std::string(key), slot, StoredObject{ObjectState::kLive, m.meta, {}}, true, {}};
    switch (m.meta.type) {
      case ValueType::kList:
        st = DigestList(view, c, d);
        break;
      case ValueType::kSet:
        st = DigestSet(view, c, d);
        break;
      case ValueType::kZSet:
        st = DigestZset(view, c, d);
        break;
      default:
        st = DigestHash(view, c, d);
        break;
    }
  }
  if (m.meta.expire_ms != 0) XorDigest(d, "!!expire!!");
  return st;
}

// Adds every stored key of db in `range` to final; the db id is mixed in before its first key, and not at all
// for an empty walk. with_ttl: M7b D23.
Status DigestMetas(ReadView& view, uint8_t db, IterOptions range, bool with_ttl, Digest20& final_digest) {
  auto it = view.NewIterator(ColumnFamily::kMeta, std::move(range));
  if (!it.ok()) return it.status();
  bool first = true;
  Status st = (*it)->SeekToFirst();
  for (; st.ok() && (*it)->Valid(); st = (*it)->Next()) {
    encoding::MetaKey mk;
    if (!encoding::DecodeMetaKey((*it)->key(), &mk)) return Status::Corruption("undecodable meta key");
    auto meta = encoding::DecodeMetadata((*it)->value());
    if (!meta.ok()) return meta.status();
    if (first) {
      MixUint32(final_digest, db);
      first = false;
    }
    Digest20 d{};
    MixDigest(d, mk.user_key);
    if (Status ost = XorObjectDigest(view, db, mk.slot, mk.user_key, *meta, d); !ost.ok()) return ost;
    if (with_ttl && meta->meta.expire_ms != 0) {
      std::string ms;
      coding::PutFixed64BE(&ms, static_cast<uint64_t>(meta->meta.expire_ms));
      MixDigest(d, ms);
    }
    XorDigest(final_digest, Bytes(d));
  }
  return st;
}

Status DigestDb(ReadView& view, uint8_t db, Digest20& final_digest) {
  return DigestMetas(view, db, {encoding::DbPrefix(db), encoding::DbPrefix(static_cast<uint8_t>(db + 1))},
                     /*with_ttl=*/false, final_digest);
}

}  // namespace

Result<Digest20> DatasetDigest(ReadView& view) {
  Digest20 final_digest{};
  for (uint8_t db = 0; db < encoding::kNumDatabases; ++db) {
    if (Status st = DigestDb(view, db, final_digest); !st.ok()) return st;
  }
  return final_digest;
}

Result<Digest20> ValueDigest(ReadView& view, uint8_t db, std::string_view key) {
  Digest20 d{};
  auto raw = view.Get(ColumnFamily::kMeta, encoding::MetaKeyFor(db, key));
  if (!raw.ok()) return raw.status().IsNotFound() ? Result<Digest20>(d) : Result<Digest20>(raw.status());
  auto meta = encoding::DecodeMetadata(*raw);
  if (!meta.ok()) return meta.status();
  if (Status st = XorObjectDigest(view, db, KeySlot(key), key, *meta, d); !st.ok()) return st;
  return d;
}

Result<Digest20> SlotRangeDigest(ReadView& view, uint16_t first, uint16_t last, bool with_ttl) {
  Digest20 d{};
  if (Status st = DigestMetas(view, 0, SlotRangeBounds(first, last), with_ttl, d); !st.ok()) return st;
  return d;
}

std::string DigestHex(const Digest20& d) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(2 * d.size(), '0');
  for (size_t i = 0; i < d.size(); ++i) {
    out[2 * i] = kHex[d[i] >> 4];
    out[2 * i + 1] = kHex[d[i] & 0xf];
  }
  return out;
}

}  // namespace tideskv
