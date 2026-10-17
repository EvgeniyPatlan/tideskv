#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cluster/topology.h"
#include "common/status.h"
#include "encoding/replog_codec.h"

// What travels from a migrating source to its target (M7b D2, D3): which replog ops belong to the
// migrating slot range, the target's lazy oid remap, the expire entries a meta implies, and the chunks
// CLUSTER IMPORTDATA carries (PUT/DEL replog-v1 batches whose cum_bytes is their own size).
namespace tideskv::migrate {

enum class OpClass : uint8_t {
  kInRange,  // a meta/subkey/zscore/expire op of db 0 inside the range: shipped
  kOutside,  // another slot or db, or a range delete that misses the range: not shipped
  kFlush,    // a FLUSHDB/FLUSHALL op (1-byte DEL_PREFIX) or a range delete that reaches the range (D10)
  kDrop,     // sys and replog ops (oid leases, the migration record), malformed keys: never shipped
};

// Key bytes 1-2 (meta/subkey/zscore, the 11-byte DEL_PREFIX) or 9-10 (expire) give the slot (CI §2.2).
OpClass ClassifyOp(const encoding::ReplOp& op, cluster::SlotRange range);
// The in-range ops of one replog batch, in order; *flush becomes true when one of them is kFlush.
std::vector<encoding::ReplOp> FilterBatch(const std::vector<encoding::ReplOp>& ops, cluster::SlotRange range,
                                          bool* flush);

// src oid -> dst oid for one import (spec §8.4): a dst oid is allocated on the first sight of a src oid in
// any op, so the order of a batch's ops does not matter (D2 step 3). Oid 0 (strings) maps to itself.
class OidRemap {
 public:
  using Allocate = std::function<Result<uint64_t>()>;  // the target's OidAllocator::Next
  explicit OidRemap(Allocate allocate) : allocate_(std::move(allocate)) {}
  Result<uint64_t> Map(uint64_t src);
  std::optional<uint64_t> Find(uint64_t src) const;
  size_t size() const { return map_.size(); }
  void Clear() { map_.clear(); }

 private:
  Allocate allocate_;
  std::unordered_map<uint64_t, uint64_t> map_;
};

// D2 step 4: `op` with its oid rewritten wherever one appears (subkey/zscore keys and their 11-byte
// DEL_PREFIX, a collection meta value, an expire value). nullopt for a DEL_PREFIX of an oid never seen:
// nothing of that object reached this node. Corruption for an oid-bearing op too short to hold its oid.
Result<std::optional<encoding::ReplOp>> RemapOp(encoding::ReplOp op, OidRemap& remap);
// D2 step 5: the expire entry an already remapped meta PUT with a TTL implies (EncodeExpireKey(0, expire_ms,
// slot, key) -> ExpireValue(oid)); nullopt for any other op or a meta without a TTL.
Result<std::optional<encoding::ReplOp>> DerivedExpire(const encoding::ReplOp& meta_put);
// D2 step 2: Corruption unless `op` is a PUT or DEL of meta/subkey/zscore/expire, or an 11-byte subkey or
// zscore DEL_PREFIX, of db 0 inside `range`. A defence against a broken or hostile source.
Status CheckImportOp(const encoding::ReplOp& op, cluster::SlotRange range);

// D3: replog-v1 batches of at most max_bytes of op bytes (key + value + 16 each); a larger op goes alone.
std::vector<std::string> EncodeChunks(const std::vector<encoding::ReplOp>& ops, size_t max_bytes);
Result<std::vector<encoding::ReplOp>> DecodeChunk(std::string_view chunk);  // CRC and structure checked

}  // namespace tideskv::migrate
