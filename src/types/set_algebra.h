#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "storage/storage.h"
#include "types/oid_allocator.h"

// SINTER/SUNION/SDIFF, their STORE variants and SINTERCARD (Redis 7.2 t_set.c semantics).
// Every key is type-checked before anything is computed: one live non-set key answers
// UserError(kErrWrongType) even if an earlier key is missing. Missing keys are empty sets.
namespace tideskv {

enum class SetOp : uint8_t { kInter, kUnion, kDiff };

// The combined members in byte order. kDiff is keys[0] minus all later keys.
Result<std::vector<std::string>> SetCombine(ReadView& view, uint8_t db, SetOp op,
                                            const std::vector<std::string_view>& keys, int64_t now_ms);
// *STORE: computes first (dst may be a source), then replaces dst whatever its type (its TTL
// goes too) with a new set, or deletes dst for an empty result. Returns the result size.
Result<int64_t> SetCombineStore(WriteTxn& txn, OidAllocator& oids, uint8_t db, SetOp op, std::string_view dst,
                                const std::vector<std::string_view>& keys, int64_t now_ms);
// SINTERCARD: the intersection size, counting stops at limit (0 = no limit).
Result<int64_t> SetInterCard(ReadView& view, uint8_t db, const std::vector<std::string_view>& keys,
                             uint64_t limit, int64_t now_ms);

}  // namespace tideskv
