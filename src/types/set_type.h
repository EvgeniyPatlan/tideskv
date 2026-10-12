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

// SPOP key: one random member, or nullopt for a missing key.
Result<std::optional<std::string>> SetPopOne(WriteTxn& txn, uint8_t db, std::string_view key,
                                             std::mt19937_64& rng, int64_t now_ms);
// SPOP key count: min(count, size) distinct random members, removed with point deletes; the key
// goes with its last member.
Result<std::vector<std::string>> SetPop(WriteTxn& txn, uint8_t db, std::string_view key, uint64_t count,
                                        std::mt19937_64& rng, int64_t now_ms);
// SRANDMEMBER key: nullopt for a missing key.
Result<std::optional<std::string>> SetRandomMember(ReadView& view, uint8_t db, std::string_view key,
                                                   std::mt19937_64& rng, int64_t now_ms);
// SRANDMEMBER key count (count != INT64_MIN): see RandomElements.
Result<std::vector<std::string>> SetRandomMembers(ReadView& view, uint8_t db, std::string_view key,
                                                  int64_t count, std::mt19937_64& rng, int64_t now_ms);
// SMOVE (Redis 7.2 smoveCommand order): a missing src is 0; then src and dst must be sets;
// src == dst answers membership; otherwise 1 if member left src (and joined dst).
Result<int64_t> SetMove(WriteTxn& txn, OidAllocator& oids, uint8_t db, std::string_view src,
                        std::string_view dst, std::string_view member, int64_t now_ms);

}  // namespace tideskv
