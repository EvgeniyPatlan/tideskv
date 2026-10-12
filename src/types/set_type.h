#pragma once
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"
#include "storage/storage.h"
#include "types/oid_allocator.h"

// Set type logic (spec §4.4) on the collection layer: a member is a subkey with an empty value.
// Every function answers UserError(kErrWrongType) for a live key of another type.
namespace tideskv {

// SADD: the number of members that were not present. Existing members are not rewritten.
Result<int64_t> SetAdd(WriteTxn& txn, OidAllocator& oids, uint8_t db, std::string_view key,
                       const std::vector<std::string_view>& members, int64_t now_ms);
// SREM: the number of distinct present members removed; the key goes with its last member.
Result<int64_t> SetRemove(WriteTxn& txn, uint8_t db, std::string_view key,
                          const std::vector<std::string_view>& members, int64_t now_ms);
Result<int64_t> SetCard(ReadView& view, uint8_t db, std::string_view key, int64_t now_ms);
Result<bool> SetIsMember(ReadView& view, uint8_t db, std::string_view key, std::string_view member,
                         int64_t now_ms);
// SMISMEMBER: 1/0 per member; a missing key gives all zeros.
Result<std::vector<int64_t>> SetMultiIsMember(ReadView& view, uint8_t db, std::string_view key,
                                              const std::vector<std::string_view>& members, int64_t now_ms);
// SMEMBERS, in byte order.
Result<std::vector<std::string>> SetMembers(ReadView& view, uint8_t db, std::string_view key, int64_t now_ms);

}  // namespace tideskv
