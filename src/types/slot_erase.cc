#include "types/slot_erase.h"

#include "encoding/key_codec.h"
#include "encoding/meta_codec.h"
#include "types/slot_keys.h"

namespace tideskv {

Result<ExpireDeletes> CollectExpireDeletes(ReadView& view, uint16_t first, uint16_t last,
                                           std::string_view after, size_t limit) {
  ExpireDeletes out;
  auto it = view.NewIterator(ColumnFamily::kMeta, SlotRangeBounds(first, last));
  if (!it.ok()) return it.status();
  Status st = after.empty() ? (*it)->SeekToFirst() : (*it)->Seek(after);
  if (st.ok() && !after.empty() && (*it)->Valid() && (*it)->key() == after) st = (*it)->Next();
  for (; st.ok() && (*it)->Valid(); st = (*it)->Next()) {
    if (out.ops.size() >= limit) return out;  // more to walk: resume after the last key
    encoding::MetaKey mk;
    if (!encoding::DecodeMetaKey((*it)->key(), &mk)) return Status::Corruption("undecodable meta key");
    auto meta = encoding::DecodeMetadata((*it)->value());
    if (!meta.ok()) return meta.status();
    out.resume_after = std::string((*it)->key());
    if (meta->meta.expire_ms == 0) continue;
    out.ops.push_back(
        {encoding::ReplOpType::kDelete, ColumnFamily::kExpire,
         encoding::EncodeExpireKey(0, static_cast<uint64_t>(meta->meta.expire_ms), mk.slot, mk.user_key),
         ""});
  }
  if (!st.ok() && !st.IsNotFound()) return st;
  out.done = true;
  return out;
}

std::vector<encoding::ReplOp> SlotRangeDeletes(uint16_t first, uint16_t last) {
  const IterOptions bounds = SlotRangeBounds(first, last);
  std::vector<encoding::ReplOp> ops;
  for (ColumnFamily cf : {ColumnFamily::kMeta, ColumnFamily::kSubkey, ColumnFamily::kZScore}) {
    ops.push_back({encoding::ReplOpType::kDeleteRange, cf, bounds.lower, bounds.upper});
  }
  return ops;
}

}  // namespace tideskv
