#include "types/index_digest.h"

#include <string>

#include "common/coding.h"
#include "common/sha1.h"
#include "encoding/key_codec.h"
#include "encoding/meta_codec.h"

namespace tideskv {
namespace {

void Fold(Digest20& d, std::string_view bytes) {
  const Sha1Digest h = Sha1(bytes);
  for (size_t i = 0; i < d.size(); ++i) d[i] ^= h[i];
}

Status CheckOneMeta(ReadView& view, const encoding::MetaKey& mk, uint64_t expire_ms, IndexDigest& out) {
  std::string rec("M");
  rec.push_back(static_cast<char>(mk.db));
  coding::PutFixed64BE(&rec, expire_ms);
  rec.append(mk.user_key);
  Fold(out.digest, rec);
  auto entry =
      view.Get(ColumnFamily::kExpire, encoding::EncodeExpireKey(mk.db, expire_ms, mk.slot, mk.user_key));
  if (!entry.ok() && !entry.status().IsNotFound()) return entry.status();
  if (!entry.ok()) ++out.missing;
  return Status::Ok();
}

Status WalkMeta(ReadView& view, IndexDigest& out) {
  auto it = view.NewIterator(ColumnFamily::kMeta);
  if (!it.ok()) return it.status();
  Status st = (*it)->SeekToFirst();
  for (; st.ok() && (*it)->Valid(); st = (*it)->Next()) {
    encoding::MetaKey mk;
    if (!encoding::DecodeMetaKey((*it)->key(), &mk)) return Status::Corruption("undecodable meta key");
    auto meta = encoding::DecodeMetadata((*it)->value());
    if (!meta.ok()) return meta.status();
    if (meta->meta.expire_ms == 0) continue;
    if (Status s = CheckOneMeta(view, mk, static_cast<uint64_t>(meta->meta.expire_ms), out); !s.ok())
      return s;
  }
  return st;
}

Status CheckOneEntry(ReadView& view, std::string_view key, std::string_view value, IndexDigest& out) {
  ++out.entries;
  std::string rec("E");
  coding::PutFixed32BE(&rec, static_cast<uint32_t>(key.size()));
  rec.append(key).append(value);
  Fold(out.digest, rec);
  encoding::ExpireKey ek;
  if (!encoding::DecodeExpireKey(key, &ek)) {
    ++out.orphans;
    return Status::Ok();
  }
  auto raw = view.Get(ColumnFamily::kMeta, encoding::EncodeMetaKey(ek.db, ek.slot, ek.user_key));
  if (!raw.ok() && !raw.status().IsNotFound()) return raw.status();
  if (!raw.ok()) {
    ++out.orphans;
    return Status::Ok();
  }
  auto meta = encoding::DecodeMetadata(*raw);
  if (!meta.ok()) return meta.status();
  if (static_cast<uint64_t>(meta->meta.expire_ms) != ek.expire_ms) ++out.orphans;
  return Status::Ok();
}

Status WalkExpire(ReadView& view, IndexDigest& out) {
  auto it = view.NewIterator(ColumnFamily::kExpire);
  if (!it.ok()) return it.status();
  Status st = (*it)->SeekToFirst();
  for (; st.ok() && (*it)->Valid(); st = (*it)->Next()) {
    if (Status s = CheckOneEntry(view, (*it)->key(), (*it)->value(), out); !s.ok()) return s;
  }
  return st;
}

}  // namespace

Result<IndexDigest> ComputeIndexDigest(ReadView& view) {
  IndexDigest out;
  if (Status st = WalkMeta(view, out); !st.ok()) return st;
  if (Status st = WalkExpire(view, out); !st.ok()) return st;
  return out;
}

}  // namespace tideskv
