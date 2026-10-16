#pragma once
#include "common/status.h"
#include "storage/storage.h"

// Keyspace queries of cluster mode over raw meta records (logically expired, unreaped keys count, as Redis
// counts what its slot dict holds): the startup check (M7a D19) and CLUSTER RESET's emptiness (D11).
namespace tideskv {

Result<bool> HasKeysOutsideDb0(ReadView& view);  // a meta record in dbs 1..15
Result<bool> KeyspaceEmpty(ReadView& view);      // no meta record in any db

}  // namespace tideskv
