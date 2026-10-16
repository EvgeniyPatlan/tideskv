#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "common/status.h"
#include "storage/storage.h"

// Keyspace queries of cluster mode over raw meta records (logically expired, unreaped keys count, as Redis
// counts what its slot dict holds): the startup check (M7a D19) and CLUSTER RESET's emptiness (D11).
namespace tideskv {

Result<bool> HasKeysOutsideDb0(ReadView& view);  // a meta record in dbs 1..15
Result<bool> KeyspaceEmpty(ReadView& view);      // no meta record in any db

// CLUSTER COUNTKEYSINSLOT / GETKEYSINSLOT (D12): this node's db 0, whatever the slot's owner. O(keys in the
// slot) (Redis keeps a per-slot dict; Kvrocks iterates too).
Result<uint64_t> CountKeysInSlot(ReadView& view, uint16_t slot);
Result<std::vector<std::string>> KeysInSlot(ReadView& view, uint16_t slot, uint64_t count);  // key-byte order

}  // namespace tideskv
