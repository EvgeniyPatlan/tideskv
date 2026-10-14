#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "common/status.h"
#include "storage/storage.h"

// DEBUG DIGEST / DIGEST-VALUE with Redis 7.2's exact algorithm (debug.c computeDatasetDigest and
// xorObjectDigest, redis72-replication §10), so a TidesKV dataset can be compared byte for byte with a
// Redis one, and a replica with its master. Logical keys and elements only: oids, the replog and the
// sys CF never contribute.
namespace tideskv {

using Digest20 = std::array<uint8_t, 20>;

// computeDatasetDigest over every stored key of dbs 0..15, logically expired ones included. The empty
// dataset is all zeros. Pass a snapshot view so the walk is point-in-time.
Result<Digest20> DatasetDigest(ReadView& view);
// xorObjectDigest for one stored key (no key name, no db), expired or not; zeros when the key is missing.
Result<Digest20> ValueDigest(ReadView& view, uint8_t db, std::string_view key);
std::string DigestHex(const Digest20& d);  // 40 lowercase hex characters

}  // namespace tideskv
