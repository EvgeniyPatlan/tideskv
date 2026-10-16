#pragma once
#include <cstdint>

#include "common/status.h"
#include "storage/storage.h"
#include "types/digest.h"

// DEBUG DIGEST-INDEX (M7a D23, a TidesKV extension): what DEBUG DIGEST deliberately leaves out (Redis mixes
// only the constant "!!expire!!" per key with a TTL): the exact deadlines and the expire index itself. Two
// nodes with the same op history have the same result; `missing` must be 0 on any node (a lost expire entry
// is a key that never expires). Stale entries (`orphans`) are legal on a master (spec §4.5).
namespace tideskv {

struct IndexDigest {
  Digest20 digest{};     // xor of SHA1 over each meta-with-TTL (db, deadline, key) and each raw expire entry
  uint64_t entries = 0;  // expire-CF entries
  uint64_t missing = 0;  // metas with a TTL whose entry [db][expire_ms][slot][key] is absent
  uint64_t orphans = 0;  // expire entries with no meta of that db, key and deadline
};

// Walks the meta and expire CFs of every db; pass a snapshot view.
Result<IndexDigest> ComputeIndexDigest(ReadView& view);

}  // namespace tideskv
