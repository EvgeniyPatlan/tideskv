#include "migrate/migrate_frames.h"

#include "common/coding.h"
#include "encoding/key_codec.h"
#include "encoding/meta_codec.h"
#include "encoding/object_codec.h"
#include "types/object_store.h"

namespace tideskv::migrate {
namespace {

using encoding::ReplOp;
using encoding::ReplOpType;

constexpr size_t kOpOverhead = 16;  // type, cf and the two varints: an upper bound (full_sync_frames.cc)

bool InRange(uint16_t slot, cluster::SlotRange r) { return slot >= r.start && slot <= r.end; }

std::string RangeText(cluster::SlotRange r) {
  return r.start == r.end ? std::to_string(r.start) : std::to_string(r.start) + "-" + std::to_string(r.end);
}

// [0][first] .. [0][last+1): the meta/subkey/zscore keys of the range (valid for 16383, slot_keys.cc).
std::string Lo(cluster::SlotRange r) { return encoding::MetaSlotPrefix(0, r.start); }
std::string Hi(cluster::SlotRange r) { return encoding::MetaSlotPrefix(0, static_cast<uint16_t>(r.end + 1)); }

OpClass ClassifySlotPrefixed(const ReplOp& op, cluster::SlotRange r) {
  if (op.type == ReplOpType::kDeleteRange) {
    const bool misses = (!op.value.empty() && op.value <= Lo(r)) || op.key >= Hi(r);
    return misses ? OpClass::kOutside : OpClass::kFlush;
  }
  if (op.key.empty()) return OpClass::kDrop;
  if (op.key[0] != 0) return OpClass::kOutside;  // another db
  // A DEL_PREFIX of 2 bytes or less covers the whole db-0 slot space (FLUSHDB of db 0): a flush, even though
  // the range is only part of what it deletes.
  if (op.key.size() < 3) return op.type == ReplOpType::kDeletePrefix ? OpClass::kFlush : OpClass::kDrop;
  return InRange(coding::DecodeFixed16BE(op.key.data() + 1), r) ? OpClass::kInRange : OpClass::kOutside;
}

// Expire keys are [db][expire_ms u64][slot u16][key]: any db-0 range delete may reach the range.
OpClass ClassifyExpire(const ReplOp& op, cluster::SlotRange r) {
  if (op.key.empty()) return OpClass::kDrop;
  if (op.key[0] != 0) return OpClass::kOutside;
  if (op.type == ReplOpType::kDeleteRange || op.type == ReplOpType::kDeletePrefix) return OpClass::kFlush;
  if (op.key.size() < 11) return OpClass::kDrop;
  return InRange(coding::DecodeFixed16BE(op.key.data() + 9), r) ? OpClass::kInRange : OpClass::kOutside;
}

Result<std::optional<ReplOp>> RemapObjectKey(ReplOp op, OidRemap& remap) {
  uint64_t src = 0;
  if (!encoding::PeekObjectOid(op.key, &src))
    return Status::Corruption("migration: an object key without oid");
  uint64_t dst = 0;
  if (op.type == ReplOpType::kDeletePrefix) {
    const std::optional<uint64_t> known = remap.Find(src);
    if (!known) return std::optional<ReplOp>();  // nothing of that object was imported
    dst = *known;
  } else if (Status st = MoveInto(remap.Map(src), &dst); !st.ok()) {
    return st;
  }
  encoding::RewriteObjectOid(&op.key, dst);
  return std::optional<ReplOp>(std::move(op));
}

Result<std::optional<ReplOp>> RemapMeta(ReplOp op, OidRemap& remap) {
  uint64_t src = 0;
  if (op.type != ReplOpType::kPut || !encoding::PeekMetaOid(op.value, &src)) {
    return std::optional<ReplOp>(std::move(op));  // a delete, or a string: no oid
  }
  uint64_t dst = 0;
  if (Status st = MoveInto(remap.Map(src), &dst); !st.ok()) return st;
  encoding::RewriteMetaOid(&op.value, dst);
  return std::optional<ReplOp>(std::move(op));
}

Result<std::optional<ReplOp>> RemapExpire(ReplOp op, OidRemap& remap) {
  if (op.type != ReplOpType::kPut) return std::optional<ReplOp>(std::move(op));
  uint64_t src = 0;
  if (!DecodeExpireValue(op.value, &src)) return Status::Corruption("migration: a malformed expire value");
  uint64_t dst = 0;
  if (Status st = MoveInto(remap.Map(src), &dst); !st.ok()) return st;
  op.value = ExpireValue(dst);
  return std::optional<ReplOp>(std::move(op));
}

}  // namespace

OpClass ClassifyOp(const ReplOp& op, cluster::SlotRange range) {
  switch (op.cf) {
    case ColumnFamily::kMeta:
    case ColumnFamily::kSubkey:
    case ColumnFamily::kZScore:
      return ClassifySlotPrefixed(op, range);
    case ColumnFamily::kExpire:
      return ClassifyExpire(op, range);
    default:
      return OpClass::kDrop;
  }
}

std::vector<ReplOp> FilterBatch(const std::vector<ReplOp>& ops, cluster::SlotRange range, bool* flush) {
  std::vector<ReplOp> out;
  for (const ReplOp& op : ops) {
    const OpClass c = ClassifyOp(op, range);
    if (c == OpClass::kFlush) *flush = true;
    if (c == OpClass::kInRange) out.push_back(op);
  }
  return out;
}

Result<uint64_t> OidRemap::Map(uint64_t src) {
  if (src == 0) return uint64_t{0};
  if (auto it = map_.find(src); it != map_.end()) return it->second;
  auto dst = allocate_();
  if (!dst.ok()) return dst.status();
  map_.emplace(src, *dst);
  return *dst;
}

std::optional<uint64_t> OidRemap::Find(uint64_t src) const {
  if (src == 0) return uint64_t{0};
  const auto it = map_.find(src);
  if (it == map_.end()) return std::nullopt;
  return it->second;
}

Result<std::optional<ReplOp>> RemapOp(ReplOp op, OidRemap& remap) {
  switch (op.cf) {
    case ColumnFamily::kSubkey:
    case ColumnFamily::kZScore:
      return RemapObjectKey(std::move(op), remap);
    case ColumnFamily::kMeta:
      return RemapMeta(std::move(op), remap);
    case ColumnFamily::kExpire:
      return RemapExpire(std::move(op), remap);
    default:
      return std::optional<ReplOp>(std::move(op));
  }
}

Result<std::optional<ReplOp>> DerivedExpire(const ReplOp& put) {
  if (put.cf != ColumnFamily::kMeta || put.type != ReplOpType::kPut) return std::optional<ReplOp>();
  encoding::MetaKey mk;
  if (!encoding::DecodeMetaKey(put.key, &mk)) return Status::Corruption("migration: an undecodable meta key");
  auto meta = encoding::DecodeMetadata(put.value);
  if (!meta.ok()) return meta.status();
  if (meta->meta.expire_ms == 0) return std::optional<ReplOp>();
  return std::optional<ReplOp>(ReplOp{
      ReplOpType::kPut, ColumnFamily::kExpire,
      encoding::EncodeExpireKey(mk.db, static_cast<uint64_t>(meta->meta.expire_ms), mk.slot, mk.user_key),
      ExpireValue(meta->meta.oid)});
}

Status CheckImportOp(const ReplOp& op, cluster::SlotRange range) {
  const bool keyspace = op.cf == ColumnFamily::kMeta || op.cf == ColumnFamily::kSubkey ||
                        op.cf == ColumnFamily::kZScore || op.cf == ColumnFamily::kExpire;
  const bool point = op.type == ReplOpType::kPut || op.type == ReplOpType::kDelete;
  const bool object_prefix = op.type == ReplOpType::kDeletePrefix &&
                             (op.cf == ColumnFamily::kSubkey || op.cf == ColumnFamily::kZScore) &&
                             op.key.size() == encoding::kObjectPrefixSize;
  if (!keyspace || (!point && !object_prefix) || ClassifyOp(op, range) != OpClass::kInRange) {
    return Status::Corruption("migration: an imported op outside slot(s) " + RangeText(range));
  }
  return Status::Ok();
}

std::vector<std::string> EncodeChunks(const std::vector<ReplOp>& ops, size_t max_bytes) {
  std::vector<std::string> out;
  std::vector<ReplOp> chunk;
  size_t bytes = 0;
  for (const ReplOp& op : ops) {
    const size_t size = op.key.size() + op.value.size() + kOpOverhead;
    if (!chunk.empty() && bytes + size > max_bytes) {
      out.push_back(encoding::EncodeReplogBatch(chunk, /*timestamp_ms=*/0, /*cum_bytes_before=*/0));
      chunk.clear();
      bytes = 0;
    }
    chunk.push_back(op);
    bytes += size;
  }
  if (!chunk.empty()) out.push_back(encoding::EncodeReplogBatch(chunk, 0, 0));
  return out;
}

Result<std::vector<ReplOp>> DecodeChunk(std::string_view chunk) {
  auto batch = encoding::DecodeReplogBatch(chunk);
  if (!batch.ok()) return batch.status();
  return std::move(batch->ops);
}

}  // namespace tideskv::migrate
