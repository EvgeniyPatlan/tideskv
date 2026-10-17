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

// [0][first] .. [0][last+1): the db-0 keys of meta, subkey and zscore in slots first..last (M7b D2). The
// upper bound of 16383 is [0][0x40][0x00], after every slot.
IterOptions SlotRangeBounds(uint16_t first, uint16_t last);

}  // namespace tideskv
