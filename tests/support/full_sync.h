#pragma once
#include <string>

#include "support/resp_client.h"

// Shared by the integration tests that need a real full sync (M6b PF1).
namespace tideskv::test {

// An empty replica of a master with an untrimmed log gets +CONTINUE (D18), so the full sync would not
// happen. A key written as a standalone node gives the replica its own replid, which the master does not
// know: its PSYNC is answered +FULLRESYNC. Call it before REPLICAOF. Returns true when the write took.
inline bool ForceFullSync(RespClient& replica) {
  return replica.Command({"SET", "standalone-key", "standalone"}).str == "OK";
}

// Writes `count` small keys "<prefix><i>" = "v" on `node`. Returns true when all of them took.
inline bool FillKeys(RespClient& node, int count, const std::string& prefix = "k") {
  for (int i = 0; i < count; ++i) {
    if (node.Command({"SET", prefix + std::to_string(i), "v"}).str != "OK") return false;
  }
  return true;
}

}  // namespace tideskv::test
